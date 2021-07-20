typedef MIDIINCAPS MidiDeviceInfo;
typedef HMIDIIN MidiHandle;

#define KEY_ON 144
#define KEY_OFF 128
#define BASE_MIDI_NOTE 69 // A4
#define MAX_MIDI_VELOCITY 127.f
#define POLYPHONIC_COUNT 16

typedef union MidiMessage {
    unsigned int message;
    unsigned char data[4];
} MidiMessage;

typedef struct MidiKey {
    unsigned char is_on;
    unsigned char note;
    double velocity_ratio;
} MidiKey;

typedef struct MidiKeyArray {
    MidiKey data[POLYPHONIC_COUNT];
    int count;
} MidiKeyArray;

static void CALLBACK SynthMidiHandler(MidiHandle midi, unsigned int msg_type, unsigned int *user_data, unsigned int param1, unsigned int param2) {
    switch(msg_type) {
        case MIM_DATA: {
            MidiKeyArray *keys = (MidiKeyArray*)user_data;
            MidiMessage msg = {param1};
            unsigned char midi_event = msg.data[0];
            unsigned char midi_note = msg.data[1];
            unsigned char midi_velocity = msg.data[2];
            unsigned int midi_timestamp = param2;
            if (midi_event == KEY_ON) {
                MidiKey *key = 0;
                for (int i = 0; i < keys->count; i++) {
                    if (keys->data[i].is_on == 0) { key = keys->data + i; break; }
                }
                if (key) {
                    key->is_on = 1;
                    key->note = midi_note;
                    key->velocity_ratio = (float)midi_velocity / MAX_MIDI_VELOCITY;
                }
            }
            else if (midi_event == KEY_OFF) {
                MidiKey *key = 0;
                for (int i = 0; i < keys->count; i++) {
                    if (keys->data[i].is_on && keys->data[i].note == midi_note) { keys->data[i].is_on = false; break; }
                }
            }
            break;
        }
    }
}

MidiHandle SynthMidiInit(int selected_device_id, MidiKeyArray *keys) {
    int num_midi_devices = midiInGetNumDevs();
    for(int device_id = 0; device_id < num_midi_devices; device_id++) {
        MidiDeviceInfo device_info;
        unsigned int result = midiInGetDevCaps(device_id, &device_info, sizeof(MidiDeviceInfo));
        if (result != MMSYSERR_NOERROR) { printf("Could not get MIDI device info\n"); return 0; }
        printf("%i: MIDI device: %s\n", device_id, device_info.szPname);
    }
    printf("Enter device id (0 - %i):", (num_midi_devices - 1));
    scanf("%d", &selected_device_id);
    printf("\n\n");
    MidiHandle midi;
    unsigned int result = midiInOpen(&midi, selected_device_id, (DWORD_PTR)SynthMidiHandler, (DWORD_PTR)keys, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) { printf("Could not open MIDI device %d.\n", selected_device_id); return 0; }
    result = midiInStart(midi);
    if (result != MMSYSERR_NOERROR) { printf("Could not start MIDI device %d.\n", selected_device_id); return 0; }    
    return midi;
}

void SynthMidiStop(MidiHandle midi) {
    unsigned int result = midiInStop(midi);
    if (result != MMSYSERR_NOERROR) { printf("Could not stop MIDI device.\n"); return; }
    
    result = midiInClose(midi);
    if (result != MMSYSERR_NOERROR) { printf("Could not close MIDI device.\n"); return; }
}
