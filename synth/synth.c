#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <float.h>
#include <limits.h>

#include "raylib.h"
#define RAYGUI_SUPPORT_ICONS
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"
#include "minimal_windows.h"
#include "synth_platform.h"
#include "midi.h"

#define SYNTH_SLOW 1 // run assertions.
#define SCREEN_WIDTH 1024
#define SCREEN_HEIGHT 768
#define SAMPLE_RATE 96000
#define SAMPLE_DURATION (1.0f / SAMPLE_RATE)
#define STREAM_BUFFER_SIZE 2048
#define MAX_OSCILLATORS 32
#define MAX_UI_OSCILLATORS 32
#define UI_PANEL_WIDTH 350
#define BASE_NOTE_FREQ 440

static MidiKeyArray midi_keys = {0};
static MidiHandle midi_input_handle = 0;

typedef float (*WaveShapeFn)(const float duty_cycle, const float phase_ratio, const float shape_param);

#define WAVE_SHAPE_OPTIONS "None;Sine;Sawtooth;Square;Triangle;Rounded Square;Noise"
typedef enum WaveShape {
    WaveShape_NONE = 0,
    WaveShape_SINE = 1,
    WaveShape_SAWTOOTH = 2,
    WaveShape_SQUARE = 3,
    WaveShape_TRIANGLE = 4,
    WaveShape_ROUNDEDSQUARE = 5,
    WaveShape_NOISE = 6,
    WaveShape_COUNT
} WaveShape;

typedef struct UiOscillator {
    float freq;
    float amplitude_ratio;
    float shape_parameter_0;
    WaveShape shape;
    bool is_dropdown_open;
    Rectangle shape_dropdown_rect;
    unsigned char modulation_state; // 0 = no modulation.
    float modulation_ratio;
} UiOscillator;

typedef struct Oscillator {
    float duty_cycle;
    float phase_ratio;
    float freq;
    float amplitude_ratio;
    float shape_parameter_0;
    unsigned short ui_id;
    bool is_modulator;
    float buffer[STREAM_BUFFER_SIZE];
} Oscillator;

typedef struct OscillatorArray {
    Oscillator osc[MAX_OSCILLATORS];
    unsigned short count;
    WaveShapeFn wave_shape_fn;
} OscillatorArray;

typedef struct ModulationPair {
    Oscillator *modulator;
    Oscillator *carrier;
    unsigned short modulation_id;
    float modulation_ratio;
} ModulationPair;

typedef struct ModulationPairArray {
    ModulationPair data[MAX_OSCILLATORS];
    unsigned short count;
} ModulationPairArray;

typedef struct Synth {
    OscillatorArray oscillator_groups[WaveShape_COUNT-1];
    unsigned short oscillator_groups_count;
    float *signal;
    unsigned short signal_count;
    float audio_frame_duration;
    UiOscillator ui_oscillator[MAX_UI_OSCILLATORS];
    unsigned short ui_oscillator_count;
    ModulationPairArray modulation_pairs;
} Synth;

typedef struct Instrument {
} Instrument;

static float FrequencyFromSemitone(float semitone) {
    return powf(2.f, semitone/12.f) * BASE_NOTE_FREQ;
}

static float SemitoneFromFrequency(float freq) {
    return 12.f * Log2f(freq / BASE_NOTE_FREQ);
}

static Oscillator* NextOscillator(OscillatorArray* osc_array) {
    Assert(osc_array->count < MAX_OSCILLATORS);
    return osc_array->osc + (osc_array->count++);
}

static void ClearOscillatorArray(OscillatorArray* osc_array) {
    osc_array->count = 0;
}

static void UpdatePhase(float *duty_cycle, float *phase_ratio, float freq, float freq_mod) {
    *phase_ratio = ((freq + freq_mod) * SAMPLE_DURATION);
    *duty_cycle += *phase_ratio;
    if (*duty_cycle < 0.0f)        *duty_cycle += 1.0f;
    if (*duty_cycle >= 1.0f)       *duty_cycle -= 1.0f;
}

static void UpdatePhaseInOsc(Oscillator *osc) {
    osc->phase_ratio = ((osc->freq) * SAMPLE_DURATION);
    osc->duty_cycle += osc->phase_ratio;
    if (osc->duty_cycle < 0.0f)        osc->duty_cycle += 1.0f;
    if (osc->duty_cycle >= 1.0f)       osc->duty_cycle -= 1.0f;
}

static void ZeroSignal(float* signal) {
    for(int t = 0; t < STREAM_BUFFER_SIZE; t++) signal[t] = 0.0f;
}

static float BandlimitedRipple(float duty_cycle, float phase_ratio) {
    if (duty_cycle < phase_ratio) {
        duty_cycle /= phase_ratio;
        return (duty_cycle + duty_cycle) - (duty_cycle * duty_cycle) - 1.0f;
    } else if (duty_cycle > 1.0f - phase_ratio) {
        duty_cycle = (duty_cycle - 1.0f) / phase_ratio;
        return (duty_cycle*duty_cycle) + (duty_cycle+duty_cycle) + 1.0f;
    } else return 0.0f;
}

// NOTE(luke): Remove this before next episode
inline float SineFast(float x) {
    //x = fmodf(x, 2*PI);
    float a = 0.083f;
    float a2 = 9.424778f * a;
    float a3 = 19.739209f * a;
    float xx = x*x;
    float xxx = xx*x;
    return (a * xxx) - (a2 * xx) + (a3 * x);
}

static float SineShape(const float duty_cycle, const float phase_ratio, const float shape_param) {
    return SineFast(2.f * PI * duty_cycle);
}

static float SawtoothShape(const float duty_cycle, const float phase_ratio, const float shape_param) {
    float sample = (duty_cycle * 2.0f) - 1.0f;
    sample -= BandlimitedRipple(duty_cycle, phase_ratio);
    return sample;
}

static float TriangleShape(const float duty_cycle, const float phase_ratio, const float shape_param) {
    float sample;
    if (duty_cycle < 0.5f) sample = (duty_cycle * 4.0f) - 1.0f; else sample = (duty_cycle * -4.0f) + 3.0f;
    return sample;
}

static float SquareShape(const float duty_cycle, const float phase_ratio, const float shape_param) {
    float dt_cycle = shape_param;
    float sample = (duty_cycle < dt_cycle) ? 1.0f : -1.0f;
    sample += BandlimitedRipple(duty_cycle, phase_ratio);
    sample -= BandlimitedRipple(fmodf(duty_cycle + (1.f - dt_cycle), 1.0f), phase_ratio);
    return sample;
}

static float RoundedSquareShape(const float duty_cycle, const float phase_ratio, const float shape_param) {
    float s = (shape_param * 8.f) + 2.f;
    float base = (float)fabs(s);
    float power = s * sinf(duty_cycle * PI * 2);
    float denominator = powf(base, power) + 1.f;
    float sample = (2.f / denominator) - 1.f;
    return sample;
}

static float NoiseShape(const float duty_cycle, const float phase_ratio, const float shape_param) {
    float dt_cycle = shape_param;
    float sample = (duty_cycle == dt_cycle) ? Randomfloat(1) : 0;
    sample += BandlimitedRipple(duty_cycle, phase_ratio);
    sample -= BandlimitedRipple(fmodf(duty_cycle + (1.f - dt_cycle), 1.0f), phase_ratio);
    return sample;
}

static void UpdateOscArray(OscillatorArray *osc_array, ModulationPairArray *mod_array) {
    for (int i = 0; i < osc_array->count; i++) {
        Oscillator *osc = &(osc_array->osc[i]);
        if (osc->freq > (SAMPLE_RATE/2) || osc->freq < -(SAMPLE_RATE/2)) continue;        
        ModulationPair *modulation = 0;
        for(int mod_i = 0; mod_i < mod_array->count; mod_i++) {
            if (mod_array->data[mod_i].carrier == osc) {  modulation = &mod_array->data[mod_i]; break; }
        }        
        for(int t = 0; t < STREAM_BUFFER_SIZE; t++) {
            float freq_mod = 0.0f;
            if (modulation) freq_mod = modulation->modulator->buffer[t] * modulation->modulation_ratio;            
            UpdatePhase(&osc->duty_cycle, &osc->phase_ratio, osc->freq, freq_mod);
            float sample = osc_array->wave_shape_fn(osc->duty_cycle, osc->phase_ratio, osc->shape_parameter_0);
            sample *= osc->amplitude_ratio;
            if (sample > 1.0f) sample = 1.0f; else if (sample < -1.0f) sample = -1.0f; // brick wall
            osc->buffer[t] = sample;
        }
    }
}

static void AccumulateOscillatorsIntoSignal(Synth *synth) {
    for (int i = 0; i < synth->oscillator_groups_count; i++) {
        OscillatorArray *osc_array = &synth->oscillator_groups[i];
        for (int osc_i = 0; osc_i < osc_array->count; osc_i++) {
            Oscillator *osc = &(osc_array->osc[osc_i]);
            if (osc->is_modulator) continue;
            for (int t = 0; t < STREAM_BUFFER_SIZE; t++) {  synth->signal[t] += osc->buffer[t];  }
        }
    }
}

static void HandleAudioStream(AudioStream stream, Synth* synth) {
    float audio_frame_duration = 0.0f;
    if (IsAudioStreamProcessed(stream)) {                                                            
        const float audio_frame_start_time = GetTime();
        ZeroSignal(synth->signal);
        for (int i = 0; i < synth->oscillator_groups_count; i++) {
            OscillatorArray *osc_array = &synth->oscillator_groups[i];
            UpdateOscArray(osc_array, &synth->modulation_pairs);
        }
        AccumulateOscillatorsIntoSignal(synth);
        UpdateAudioStream(stream, synth->signal, synth->signal_count);
        synth->audio_frame_duration = GetTime() - audio_frame_start_time;
    }
}

static void DrawSignal(Synth *synth) {
    // Drawing the signal.
    int zero_crossing_index = 0;
    for (int i = 1; i < synth->signal_count; i++) {
            if (synth->signal[i] >= 0.0f && synth->signal[i-1] < 0.0f) { zero_crossing_index = i; break; }
    }
    Vector2 signal_points[STREAM_BUFFER_SIZE];
    const float screen_vertical_midpoint = (SCREEN_HEIGHT/2);
    for (int point_idx = 0; point_idx < synth->signal_count; point_idx++) {
        const int signal_idx = (point_idx + zero_crossing_index) % STREAM_BUFFER_SIZE;
        signal_points[point_idx].x = (float)point_idx + UI_PANEL_WIDTH;
        signal_points[point_idx].y = screen_vertical_midpoint + (int)(synth->signal[signal_idx] * 300);
    }
    DrawLineStrip(signal_points, STREAM_BUFFER_SIZE - zero_crossing_index, RED);
}

static void DrawUi(Synth *synth) {
    const int panel_x_start = 0;
    const int panel_y_start = 0;
    const int panel_width = UI_PANEL_WIDTH;
    const int panel_height = SCREEN_WIDTH;
    bool is_shape_dropdown_open = false;
    int shape_index = 0;
    
    GuiGrid((Rectangle){0,0,SCREEN_WIDTH,SCREEN_HEIGHT}, SCREEN_HEIGHT/8, 2);
    GuiPanel((Rectangle){ panel_x_start, panel_y_start, panel_width, panel_height });

    bool click_add_oscillator = GuiButton((Rectangle){ panel_x_start + 10, panel_y_start + 10, panel_width - 20, 25 }, "Add Oscillator");
    if (click_add_oscillator) {
        synth->ui_oscillator_count += 1;
        // Set defaults:
        UiOscillator *ui_osc = synth->ui_oscillator + (synth->ui_oscillator_count - 1);
        ui_osc->shape = WaveShape_SINE;
        ui_osc->freq = BASE_NOTE_FREQ;
        ui_osc->amplitude_ratio = 0.1f;
        ui_osc->shape_parameter_0 = 0.5f;
        ui_osc->modulation_ratio = 100.f;
    }
    
    float panel_y_offset = 0;
    for (int ui_osc_i = 0; ui_osc_i < synth->ui_oscillator_count; ui_osc_i++) {
        UiOscillator* ui_osc = &synth->ui_oscillator[ui_osc_i];
        const bool has_shape_param = (ui_osc->shape == WaveShape_SQUARE || ui_osc->shape == WaveShape_ROUNDEDSQUARE);
        
        const int osc_panel_width = panel_width - 20;
        const int osc_panel_height = has_shape_param ? 130 : 100;
        const int osc_panel_x = panel_x_start + 10;
        const int osc_panel_y = panel_y_start + 50 + panel_y_offset;
        panel_y_offset += osc_panel_height + 5;
        GuiPanel((Rectangle){ osc_panel_x, osc_panel_y, osc_panel_width, osc_panel_height });
        
        const float slider_padding = 50.f;
        const float el_spacing = 5.f;
        Rectangle el_rect = { osc_panel_x + slider_padding + 30, osc_panel_y + 10, osc_panel_width - (slider_padding * 2), 25 };

        // Frequency slider
        unsigned char freq_slider_label[16];
        sprintf(freq_slider_label, "%.1fHz", ui_osc->freq);
        float log_freq = ui_osc->freq;
        log_freq = GuiSlider(el_rect, freq_slider_label, "", log_freq, BASE_NOTE_FREQ * 0.5, BASE_NOTE_FREQ * 2.0 );
        ui_osc->freq = log_freq;
        el_rect.y += el_rect.height + el_spacing;

        // Amplitude slider
        float decibels = -60.f + (float)ui_osc->amplitude_ratio * 60.f;
        unsigned char amp_slider_label[32];
        sprintf(amp_slider_label, "%.1f dB", decibels);
        ui_osc->amplitude_ratio = GuiSlider(el_rect, amp_slider_label, "", ui_osc->amplitude_ratio, 0.f, 1.0f );
        el_rect.y += el_rect.height + el_spacing;

        // Shape parameter slider
        if (has_shape_param) {
            float shape_param = ui_osc->shape_parameter_0;
            unsigned char shape_param_label[32];
            sprintf(shape_param_label, "%.1f", shape_param);
            shape_param = GuiSlider(el_rect, shape_param_label, "", shape_param, 0.f, 1.f );
            ui_osc->shape_parameter_0 = shape_param;
            el_rect.y += el_rect.height + el_spacing;
        }

        // Defer shape drop-down box.
        ui_osc->shape_dropdown_rect = el_rect;
        el_rect.y += el_rect.height + el_spacing;

        Rectangle delete_button_rect = el_rect;
        delete_button_rect.x = osc_panel_x + 5;
        delete_button_rect.y -= el_rect.height + el_spacing;
        delete_button_rect.width = 30;
        bool is_delete_button_pressed = GuiButton(delete_button_rect, "X");
        if (is_delete_button_pressed) {
            memmove( synth->ui_oscillator + ui_osc_i, 
                    synth->ui_oscillator + ui_osc_i + 1, (synth->ui_oscillator_count - ui_osc_i) * sizeof(UiOscillator) );
            synth->ui_oscillator_count -= 1;
        }

        if (ui_osc->modulation_state) {
            float modulation_ratio = ui_osc->modulation_ratio;
            unsigned char modulation_label[32];
            sprintf(modulation_label, "MOD.1f", modulation_ratio);
            modulation_ratio = GuiSlider(el_rect, modulation_label, "", modulation_ratio, 0.f, 100.f );
            ui_osc->modulation_ratio = modulation_ratio;
            el_rect.y += el_rect.height + el_spacing;
        }
        
        Rectangle modulation_button_rect = delete_button_rect;
        modulation_button_rect.x += 40;
        char *modulation_button_text = (ui_osc->modulation_state == 0) ? "N/A" : TextFormat("%d", ui_osc->modulation_state);
        bool modulation_button_pressed = GuiButton(modulation_button_rect, modulation_button_text);
        if (modulation_button_pressed) {
            ui_osc->modulation_state += 1;
            if (ui_osc->modulation_state == 8) ui_osc->modulation_state = 0;
        }
    }
    
    for (int ui_osc_i = 0; ui_osc_i < synth->ui_oscillator_count; ui_osc_i += 1) {
        UiOscillator* ui_osc = &synth->ui_oscillator[ui_osc_i];
        // Shape select
        int shape_index = (int)(ui_osc->shape);
        bool is_dropdown_click = GuiDropdownBox(ui_osc->shape_dropdown_rect, WAVE_SHAPE_OPTIONS, &shape_index, ui_osc->is_dropdown_open );
        ui_osc->shape = (WaveShape)(shape_index);
        if (is_dropdown_click) { ui_osc->is_dropdown_open = !ui_osc->is_dropdown_open; }
        if (ui_osc->is_dropdown_open) break;
    }
}

static void ApplyUiState(Synth *synth) {
    // Reset synth
    // TODO(luke): make this something we can iterate.
    for (int i = 0; i < synth->oscillator_groups_count; i++) {
        ClearOscillatorArray(&synth->oscillator_groups[i]);
    }
    synth->modulation_pairs.count = 0;    
    for (int ui_osc_i = 0; ui_osc_i < synth->ui_oscillator_count; ui_osc_i++) {
        UiOscillator ui_osc = synth->ui_oscillator[ui_osc_i];
        
        for(int note_idx = midi_keys.count-1; note_idx >= 0; note_idx--) {
            MidiKey midi_key = midi_keys.data[note_idx];
            if (!midi_key.is_on) continue;
            Oscillator *osc = NULL;
            if (ui_osc.shape > 0 && ui_osc.shape < WaveShape_COUNT) { osc = NextOscillator(&synth->oscillator_groups[ui_osc.shape-1]); }
            
            if (osc != NULL) {
                osc->ui_id = ui_osc_i;
                float ui_semitone = SemitoneFromFrequency(ui_osc.freq);
                float midi_semitone = (float)(midi_key.note - BASE_MIDI_NOTE);
                float osc_freq = FrequencyFromSemitone(ui_semitone + midi_semitone);
                osc->freq = osc_freq;
                osc->amplitude_ratio = ui_osc.amplitude_ratio * midi_key.velocity_ratio;
                osc->shape_parameter_0 = ui_osc.shape_parameter_0;
                osc->is_modulator = false;
                if (ui_osc.modulation_state > 0 && (ui_osc.modulation_state-1) < synth->ui_oscillator_count) {
                    ModulationPair *mod_pair = synth->modulation_pairs.data + synth->modulation_pairs.count++;
                    mod_pair->modulator = 0;
                    mod_pair->carrier = osc;
                    mod_pair->modulation_id = ui_osc.modulation_state - 1;
                    mod_pair->modulation_ratio = 500.f;//ui_osc.modulation_ratio;
                }
            }
        }
    }
    
    for(int mod_i = 0; mod_i < synth->modulation_pairs.count; mod_i++) {
        ModulationPair *mod_pair = &synth->modulation_pairs.data[mod_i];
        unsigned short shape_id = (unsigned short)synth->ui_oscillator[mod_pair->modulation_id].shape;
        OscillatorArray *osc_array = &synth->oscillator_groups[shape_id - 1];
        // TODO: modulators (LFOs) should be per channel
        for (int osc_i = 0; osc_i < osc_array->count; osc_i++) {
            Oscillator *osc = &osc_array->osc[osc_i];
            if (osc->ui_id == mod_pair->modulation_id) {
                if (mod_pair->modulator == 0) mod_pair->modulator = osc;
                osc->is_modulator = true;
            }
        }
    }
}

int main(void) {
    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Synth");
    int TARGET_FPS = GetMonitorRefreshRate(GetCurrentMonitor());
    InitAudioDevice();
    midi_keys.count = ArrayCount(midi_keys.data);
    midi_input_handle = SynthMidiInit(0, &midi_keys);
    //GuiLoadStyle(".\\styles\\jungle\\jungle.rgs");

    SetAudioStreamBufferSizeDefault(STREAM_BUFFER_SIZE);
    AudioStream synth_stream = LoadAudioStream(SAMPLE_RATE, sizeof(float) * 8, 1);
    PlayAudioStream(synth_stream);

    ModulationPair modulation_pairs[256] = {0};
    float signal[STREAM_BUFFER_SIZE] = {0};

    printf("Oscillator size: %d\n", sizeof(Oscillator));
    printf("UiOscillator size: %d\n", sizeof(UiOscillator));
    printf("OscillatorArray size: %d\n", sizeof(OscillatorArray));
    printf("Synth size: %d\n", sizeof(Synth));

    Synth *synth = (Synth *)malloc(sizeof(Synth));
    synth->oscillator_groups_count = ArrayCount(synth->oscillator_groups);
    synth->signal = signal;
    synth->signal_count = ArrayCount(signal);
    synth->oscillator_groups[WaveShape_SINE-1].count = 0;
    synth->oscillator_groups[WaveShape_SINE-1].wave_shape_fn = SineShape;
    synth->oscillator_groups[WaveShape_SAWTOOTH-1].count = 0;
    synth->oscillator_groups[WaveShape_SAWTOOTH-1].wave_shape_fn = SawtoothShape;
    synth->oscillator_groups[WaveShape_TRIANGLE-1].count = 0;
    synth->oscillator_groups[WaveShape_TRIANGLE-1].wave_shape_fn = TriangleShape;
    synth->oscillator_groups[WaveShape_SQUARE-1].count = 0;
    synth->oscillator_groups[WaveShape_SQUARE-1].wave_shape_fn = SquareShape;
    synth->oscillator_groups[WaveShape_ROUNDEDSQUARE-1].count = 0;
    synth->oscillator_groups[WaveShape_ROUNDEDSQUARE-1].wave_shape_fn = RoundedSquareShape;
    synth->oscillator_groups[WaveShape_NOISE-1].count = 0;
    synth->oscillator_groups[WaveShape_NOISE-1].wave_shape_fn = NoiseShape;
    synth->modulation_pairs.count = 0;
#if 0
    synth->modulation_pairs.count = 1;
    synth->modulation_pairs.data[0].modulator = &synth->oscillator_groups[WaveShape_SINE-1].osc[0];
    synth->modulation_pairs.data[0].carrier = &synth->oscillator_groups[WaveShape_SINE-1].osc[1];
    synth->modulation_pairs.data[0].modulation_ratio = 100.0f;
#endif
    printf ("======== BEGIN MAIN LOOP\n");
    while(!WindowShouldClose()) {
        HandleAudioStream(synth_stream, synth);
        BeginDrawing();
        ClearBackground(BLACK);
        DrawUi(synth);
        ApplyUiState(synth);
        DrawSignal(synth);
        const float total_frame_duration = GetFrameTime();
        DrawText(TextFormat("Frame time: %.3f%%, Audio budget: %.3f%%", 
            (100.0f / (total_frame_duration * TARGET_FPS)), 
            100.0f / ((1.0f / synth->audio_frame_duration) / ((float)SAMPLE_RATE/STREAM_BUFFER_SIZE))),
            UI_PANEL_WIDTH + 10, 10, 20, RED);
        DrawText(TextFormat("Fundemental Freq: %.1f", synth->oscillator_groups[0].osc[0].freq), UI_PANEL_WIDTH + 10, 30, 20, RED);
        EndDrawing();
    }
    printf ("======== END MAIN LOOP\n");
    
    if (midi_input_handle) SynthMidiStop(midi_input_handle);
    UnloadAudioStream(synth_stream);
    CloseAudioDevice();
    CloseWindow();    
    return 0;
}
