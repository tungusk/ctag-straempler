#include "menu_items.h"

const char* s2_toggle_items[] = {"OFF", "ON"};
const int s2_n_toggle_items = sizeof(s2_toggle_items) / sizeof(int);

const char* s2_trig_items[] = {"Gate", "Latch"};
const int s2_n_trig_items = sizeof(s2_trig_items) / sizeof(int);

const char* s2_main_menus[] = {"Play", "Slot", "Sample", "Preset", "More"};
const int s2_n_main_menus = sizeof(s2_main_menus)/sizeof(int);

const char* s2_browse_menus[] = {"Tag", "ID", "Text", "User"};
const int s2_n_browse_menus = sizeof(s2_browse_menus)/sizeof(int);

const char* s2_play_menus[] = {"Voice 0", "Voice 1", "Recording", "External In", "Effects", "CV Matrix"};
const int s2_n_play_menus = sizeof(s2_play_menus)/sizeof(int);

const char* s2_effect_menus[] = {"Delay"};
const int s2_n_effect_menus = sizeof(s2_effect_menus) / sizeof(int);

const char* s2_voice_menus[] = {"Trig Type", "Volume", "Pan", "Pitch", "Pitch CV", "Playback Speed", "Distortion", "Drive", "Delay Send", "Filter", "ADSR", "Playmodes"};
const int s2_n_voice_menus = sizeof(s2_voice_menus) / sizeof(int);

const char* s2_externel_in_menus[] = {"EXTIN Active", "Volume", "Pan", "Delay Send"};
const int s2_n_externel_in_menus = sizeof(s2_externel_in_menus) / sizeof(int);

const char* s2_filter_menus[] = {"Filter Active", "Base" , "Width", "Q"};
const int s2_n_filter_menus = sizeof(s2_filter_menus)/ sizeof(int);

const char* s2_adsr_menus[] = {"Attack", "Decay", "Sustain", "Release", "Env"};
const int s2_n_adsr_menus = sizeof(s2_adsr_menus) / sizeof(int);

const char* s2_playmode_menus[] = {"Mode", "Start", "Loop Start", "Loop End", "Loop Position"};
const char* s2_playmode_modes[] = {"Single Shot", "Loop", "Ping Pong"};
const int s2_n_playmode_menus = sizeof(s2_playmode_menus) / sizeof(int);
const int s2_n_play_modes = sizeof(s2_playmode_modes) / sizeof(int);

const char* s2_delay_menus[] = {"Delay Active", "Delay Mode", "Delay Time", "Delay Pan", "Delay Feedback", "Delay Volume"};
const int s2_n_delay_menus = sizeof(s2_delay_menus) / sizeof(int);

const char* s2_delay_mode_items[] = {"Stereo", "Ping Pong"};
const int s2_n_delay_mode_items = sizeof(s2_toggle_items) / sizeof(int);

const char* s2_cv_matrix_items[] = {"SOURCE", "AMOUNT", "DESTINATION"};
const int s2_n_cv_matrix_items = sizeof(s2_cv_matrix_items) / sizeof(int);

const char* s2_matrix_parameter_items[] = {"None","V0 Volume", "V0 Pan", "V0 Pitch", "V0 PB Speed", "V0 Dist Drive", "V0 Dly Send", "V0 F. Base", "V0 F. Width", "V0 F. Q", "V0 Attack", 
                                                "V0 Decay", "V0 Sustain", "V0 Release", "V0 Start", "V0 Loop Start", "V0 Loop End", "V1 Volume", "V1 Pan", "V1 Pitch", "V1 PB Speed",
                                                "V1 Dist Drive", "V1 Dly Send", "V1 F. Base", "V1 F. Width", "V1 F. Q", "V1 Attack", "V1 Decay", "V1 Sustain", "V1 Release", "V1 Start",
                                                "V1 Loop Start", "V1 Loop End", "Dly Time", "Dly Pan", "Dly Feedback", "Dly Volume",
                                                "T0 Rec Arm", "T1 Rec Arm"};
const int s2_n_matrix_parameters = sizeof(s2_matrix_parameter_items) / sizeof(int);

const char* s2_preset_menus[] = {"Presets", "Banks"};
const int s2_n_preset_menus = sizeof(s2_preset_menus)/sizeof(int);

const char* s2_preset_choices[] = {"Load", "Save", "New", "Delete", "Reset"};
const int s2_n_preset_choices = sizeof(s2_preset_choices)/sizeof(int);

const char* s2_bank_choices[] = {"Load", "New", "Delete"};
const int s2_n_bank_choices = sizeof(s2_bank_choices)/sizeof(int);

const char* s2_choices[] = {"Actually No:(", "Totally Sure!"};
const int s2_n_choices = sizeof(s2_choices) / sizeof(int);

const char* s2_slot_menus[] = {"User", "Freesound"};
const int s2_n_slot_menus = sizeof(s2_slot_menus)/sizeof(int);

