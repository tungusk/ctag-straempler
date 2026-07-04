#include "adsr.h"

void s2_init_adsr(adsr_t* adsr){
    adsr->attack_time = 44100;
    adsr->adsr_state = OFF;
    adsr->decay_time = 44100;
    adsr->release_time = 88200;
    adsr->sustain_level = 0.7f;
    adsr->adsr_increment = ENV_LUT_SIZE / adsr->attack_time;
    adsr->last_val = 0.0f;
}

void s2_calculate_adsr_phase_increment(adsr_t* adsr){
    switch(adsr->adsr_state){
        case A:
            adsr->adsr_increment = ENV_LUT_SIZE / adsr->attack_time;
            break;
        case D:
            adsr->adsr_increment = ENV_LUT_SIZE / adsr->decay_time;
            break;
        case S:
            break;
        case R:
            adsr->adsr_increment = ENV_LUT_SIZE / adsr->release_time;
            break;
        case OFF:
            break;
    }
}

void s2_update_eg_state(adsr_t* adsr, int ev)
{
    switch (ev)
    {
    case 1:
        adsr->adsr_phase = 0;
        adsr->adsr_state = A;
        adsr->adsr_increment = ENV_LUT_SIZE / adsr->attack_time;
        break;
    case 2:
        adsr->adsr_phase = 0;
        adsr->adsr_state = R;
        adsr->adsr_increment = ENV_LUT_SIZE / adsr->release_time;
        break;
    default:
        break;
    }
}

void s2_get_next_eg_val(adsr_t* adsr, float* sample_left, float* sample_right)
{
    
    float eg_value = 0.0;
    int32_t pos = (int32_t) (adsr->adsr_phase);
    float alpha = (float) (adsr->adsr_phase - pos);
    float inv_alpha = 1.0f - alpha;
    float interpolated_eg = s2_env_lut_float[pos] * inv_alpha + s2_env_lut_float[pos + 1] * alpha;
    
    switch(adsr->adsr_state)
    {
        case A:
            eg_value = interpolated_eg;
            adsr->last_val = eg_value;
            if(adsr->adsr_phase >= adsr->attack_time || eg_value >= 1.0f)
            {
                adsr->last_val = 1.0f;
                eg_value = 1.0f;
                adsr->adsr_state = D;
                adsr->adsr_phase = 0;
                adsr->adsr_increment = ENV_LUT_SIZE / adsr->decay_time;
            }
            adsr->adsr_phase += adsr->adsr_increment;
            break;
        case D:
            eg_value = 1.0f - ((1.0f - adsr->sustain_level) * interpolated_eg);
            adsr->last_val = eg_value;
            if(adsr->adsr_phase >= adsr->decay_time || eg_value <= adsr->sustain_level)
            {
                adsr->last_val = adsr->sustain_level;
                eg_value = adsr->sustain_level;
                adsr->adsr_state = S;
                adsr->adsr_phase = 0;
                adsr->adsr_increment = ENV_LUT_SIZE / adsr->release_time;
            }
            adsr->adsr_phase += adsr->adsr_increment;
            break;
        case S:
            eg_value = adsr->sustain_level;
            break;
        case R:
            eg_value = adsr->last_val * (1.0f - interpolated_eg);
            if(adsr->adsr_phase >= adsr->release_time || eg_value <= 0.0f)
            {
                adsr->adsr_state = OFF;
                eg_value = 0.0f;
            }
            adsr->adsr_phase += adsr->adsr_increment;
            break;
        case OFF:
            eg_value = 0;
            break;
        default:
            break;
    }
    
    *sample_left *= eg_value;
    *sample_right *= eg_value;
    
    return;
}

void s2_process_fade(voice_t* voice, float* buffer, int buffer_size){
    
    for(int i = 0; i < buffer_size / 2; i++){
        buffer[2 * i] *= s2_fade(&voice->s2_fade[0]);
        buffer[2 * i + 1] *= s2_fade(&voice->s2_fade[1]);
    }
    
}

float s2_fade(fade_t* s2_fade){
    int32_t pos = (int32_t) (s2_fade->phase);
    float alpha = (float) (s2_fade->phase - pos);
    float inv_alpha = 1.0f - alpha;
    float interpolated_eg = s2_env_lut_float[pos] * inv_alpha + s2_env_lut_float[pos + 1] * alpha;
    float eg_value = 1.0f;

    switch(s2_fade->state)
    {
        case R:
            eg_value = (1.0f - interpolated_eg);
            
            if(s2_fade->phase >= s2_fade->fade_time || eg_value <= 0.0f)
            {
                s2_fade->state = OFF;
                eg_value = 0.0f;
            }
            
            break;
        case OFF:
            eg_value = 0.0f;
            break;
        case LISTEN:
            eg_value = 1.0f;
        default:
            break;
    }
    
    s2_fade->phase += s2_fade->increment;
    return eg_value;

}

void s2_process_fade_state(voice_t* voice, int ev){
    for(size_t i = 0; i < 2; i++)
    {
        s2_update_fade_state(&voice->s2_fade[i], ev);
    }
}

void s2_update_fade_state(fade_t* s2_fade, int ev)
{
    switch (ev)
    {
    case 1:
        s2_fade->phase = 0;
        s2_fade->state = R;
        s2_fade->increment = ENV_LUT_SIZE / s2_fade->fade_time;
        break;
    default:
        break;
    }
}

void s2_init_fade(voice_t* voice){
    for(int i = 0; i < 2; i++){
        voice->s2_fade[i].fade_time = 512;
        voice->s2_fade[i].increment = ENV_LUT_SIZE / voice->s2_fade[i].fade_time;
        voice->s2_fade[i].phase = 0;
        voice->s2_fade[i].start_val = 0.0f;
        voice->s2_fade[i].state = LISTEN;
    }
}