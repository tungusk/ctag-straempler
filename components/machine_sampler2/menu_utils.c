#include "menu_utils.h"
#include "esp_timer.h"

// encoder acceleration for the fine Q13.3 percent params (0..800 = 0..100%,
// 0.125% per unit): a deliberate click keeps the fine step, quick successive
// clicks scale up so a full sweep doesn't take 800 detents
static uint16_t accel_step(void){
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    int64_t dt = now - last_us;
    last_us = now;
    if(dt < 25000) return 16;   // fast spin: 2% per detent
    if(dt < 60000) return 4;    // brisk turn: 0.5%
    return 1;                   // slow click: 0.125%
}

void s2_incParamValue(param_data_t* data, int index, int vid, bool* pbs_state, matrix_ui_row_t* matrix, xQueueHandle handle){
    switch (index)
    {
        case SID_TRIG_TYPE:
            data->trig_mode_latch ^= true;
            break;
        case SID_VOLUME:
            if((data->volume + 1) >= 200){
                data->volume = 200;
            }else{
                data->volume += 1;
            }
            //ESP_LOGI("UI","Incremented volume %u", data->volume);
            break;
        case SID_PAN:
            if((data->pan + 128) >= 16384){ // 16384 is +1.0
                data->pan = 16384;
            }else{
                data->pan += 128; 
            }
            //ESP_LOGI("UI","Incremented pan %d", data->pan);
            break;
        case SID_PITCH:
            if((data->pitch + 1) >= 24){
                data->pitch = 24;
            }else{
                data->pitch += 1;
            }
            //ESP_LOGI("UI","Incremented volupitchme %u", data->pitch);
            break;
        case SID_PITCH_CV_ACTIVE:
            
            data->pitch_cv_active = !(data->pitch_cv_active);
            {
                // Sampler2: pitch may live on any matrix row — find it
                int want = vid ? MTX_V1_PITCH : MTX_V0_PITCH;
                for(int i = 0; i < 8; i++){
                    if(matrix[i].dst == want){
                        matrix[i].amt = (data->pitch_cv_active == 1) ? 100 : 0;
                        break;
                    }
                }
            } 
            //ESP_LOGI("UI","Changed pitch_cv_active %u", data->pitch_cv_active);
            break;
        case SID_PBSPEED:
            {
                // Sampler2: accelerated — base step 16 in Q2.14 is a
                // 4096-detent full sweep otherwise
                int d = 16 * accel_step();
                if((data->playback_speed + d) >= 32767){
                    data->playback_speed = 32767;   // ~2.0 in Q2.14 — up to double-speed playback
                }else{
                    data->playback_speed += d;
                    if(data->playback_speed > 1 && ((*pbs_state) == 0)){
                        *pbs_state = 1;
                        xQueueSend(handle, (void*) pbs_state, portMAX_DELAY);
                    }
                }
            }
            break;
        case SID_DIST_ACTIVE:
            data->dist_active = !(data->dist_active);
            //ESP_LOGI("UI","Changed is_active %u", data->dist_active);
            break;    
        case SID_DIST:
            if((data->dist_drive + 1) >=  255){
                data->dist_drive = 255;
            }else{
                data->dist_drive += 1;
            }
            //ESP_LOGI("UI","Incremented dist_drive %u", data->dist_drive);
            break;
        case SID_DELAY_SEND:
            if((data->delay_send + 1) >= 100){       
                data->delay_send = 100;
            }else{
                data->delay_send += 1;
            }
            break;
        default:
            break;
    }
}

void s2_decParamValue(param_data_t* data, int index, int vid, bool* pbs_state, matrix_ui_row_t* matrix, xQueueHandle handle){
    switch (index)
    {
        case SID_TRIG_TYPE:
            data->trig_mode_latch ^= true;
            break;
        case SID_VOLUME:
            if((data->volume - 1) <= 0){
                data->volume = 0;
            }else{
                data->volume -= 1;
            }
            //ESP_LOGI("UI","Decremented volume %u", data->volume);
            break;
        case SID_PAN:
            if((data->pan - 128) <= -16384){ // -16384 is -1.0
                data->pan = -16384;
            }else{
                data->pan -= 128;
            }
            //ESP_LOGI("UI","Decremented pan %d", data->pan);
            break;
        case SID_PITCH:
            if((data->pitch - 1) <= 0){
                data->pitch = 0;
            }else{
                data->pitch -= 1;
            }
            //ESP_LOGI("UI","Decremented pitch %u", data->pitch);
            break;
        case SID_PITCH_CV_ACTIVE:
            data->pitch_cv_active = !(data->pitch_cv_active);
            {
                // Sampler2: pitch may live on any matrix row — find it
                int want = vid ? MTX_V1_PITCH : MTX_V0_PITCH;
                for(int i = 0; i < 8; i++){
                    if(matrix[i].dst == want){
                        matrix[i].amt = (data->pitch_cv_active == 1) ? 100 : 0;
                        break;
                    }
                }
            }
            //ESP_LOGI("UI","Changed pitch_cv_active %d", data->volume);
            break;
        case SID_PBSPEED:
            {
                int d = 16 * accel_step();
                if((data->playback_speed - d) <= -32767){
                    data->playback_speed = -32767;
                }else{
                    data->playback_speed -= d;
                    if(data->playback_speed < 0 && ((*pbs_state) == 1)){
                        *pbs_state = 0;
                        xQueueSend(handle, (void*) pbs_state, portMAX_DELAY);
                    }
                }
            }
            break;
        case SID_DIST_ACTIVE:
            data->dist_active = !(data->dist_active);
            //ESP_LOGI("UI","Changed is_active %u", data->dist_active);
            break;    
        case SID_DIST:
            if((data->dist_drive -1) <=  0){       
                data->dist_drive = 0;
            }else{
                data->dist_drive -= 1;
            }
            //ESP_LOGI("UI","Decremented distortion %u", data->dist_drive);
            break;
        case SID_DELAY_SEND:
            if((data->delay_send - 1) <= 0){
                data->delay_send = 0;
            }else{
                data->delay_send -= 1;
            }
            break;
        default:
            break;
    }
}

void s2_incADSRValue(adsr_data_t* data, uint16_t* adsrIndex, int index){
    switch (index)
    {
        case SID_ATTACK:
            if(adsrIndex[0] >= 255){
                adsrIndex[0] = 255;
                data->attack = s2_msLut[adsrIndex[0]];
            }else{
                adsrIndex[0] += 1;
                data->attack = s2_msLut[adsrIndex[0]];
                
            }
            // ESP_LOGI("UI","Incremented attack %u", data->attack);       
            break;
        case SID_DECAY:
            if(adsrIndex[1] >= 255){
                adsrIndex[1] = 255;
                data->decay = s2_msLut[adsrIndex[1]];
            }else{
                adsrIndex[1] += 1;
                data->decay = s2_msLut[adsrIndex[1]];
            }
            // ESP_LOGI("UI","Incremented decay %u", data->decay);         
            break;
        case SID_SUSTAIN:
          if((data->sustain + 1) >= 100){       
                data->sustain = 100;
            }else{
                data->sustain += 1;
            }
            //ESP_LOGI("UI","Incremented sustain %u", data->sustain);
            break;    
        case SID_RELEASE:
            if(adsrIndex[2] >= 255){
                adsrIndex[2] = 255;
                data->release = s2_msLut[adsrIndex[2]];
            }else{
                adsrIndex[2] += 1;
                data->release = s2_msLut[adsrIndex[2]];
            }     
            
            break;    
        default:
            break;
    }
}

void s2_decADSRValue(adsr_data_t* data, uint16_t* adsrIndex, int index){
    switch (index)
    {
        case SID_ATTACK:
            if(adsrIndex[0] <= 0){
                adsrIndex[0] = 0;
                data->attack = s2_msLut[adsrIndex[0]];
            }else{
                adsrIndex[0]--;
                data->attack = s2_msLut[adsrIndex[0]];
            }
            // ESP_LOGI("UI","Decremented attack %u", data->attack);
            break;
        case SID_DECAY:
            if(adsrIndex[1] <= 0){
                adsrIndex[1] = 0;
                data->decay = s2_msLut[adsrIndex[1]];
            }else{
                adsrIndex[1]--;
                data->decay = s2_msLut[adsrIndex[1]];
            }  
            // ESP_LOGI("UI","Decremented decay %u", data->decay);
            break;
        case SID_SUSTAIN:
            if((data->sustain - 1) <= 0){
                data->sustain = 0;
            }else{
                data->sustain -= 1;
            }
            // ESP_LOGI("UI","Decremented sustain %u", data->sustain);
            break;
        case SID_RELEASE:
            if(adsrIndex[2] <= 0){
                adsrIndex[2] = 0;
                data->release = s2_msLut[adsrIndex[2]];
            }else{
                adsrIndex[2]--;
                data->release = s2_msLut[adsrIndex[2]];
            }  
            break;    
        default:
            break;
    }
}

void s2_incFilterValue(filter_data_t* data, int index){
    switch (index)
    {
        case SID_FILTER_ACTIVE:
            data->is_active = !(data->is_active);
            // ESP_LOGI("UI","Changed is_active %u", data->is_active);
            break;
        case SID_BASE:
            {
                int d = accel_step();
                if((data->base + d) >= 511 ){       //Dummy base value 0 - 255
                    data->base = 511;
                }else{
                    data->base += d;
                }
            }
            // ESP_LOGI("UI","Incremented base %u", data->base);
            break;
        case SID_WIDTH:
            {
                int d = accel_step();
                if((data->width + d) >= 511){       //Dummy width value 0 - 255
                    data->width = 511;
                }else{
                    data->width += d;
                }
            }
            // ESP_LOGI("UI","Incremented width %u", data->width);
            break;
        case SID_Q:
            if((data->q + 1) >= 255){           
                data->q = 255;
            }else{
                data->q += 1;
            }
            //ESP_LOGI("UI","Incremented q %u", data->q);
            break;    
        default:
            break;
    }
}

void s2_decFilterValue(filter_data_t* data, int index){
    switch (index)
    {
        case SID_FILTER_ACTIVE:
            data->is_active = !(data->is_active);
            // ESP_LOGI("UI","Changed is_active %d", data->is_active);
            break;
        case SID_BASE:
            {
                int d = accel_step();
                if((data->base - d) <= 0 ){       //Dummy base value 0 - 255
                    data->base = 0;
                }else{
                    data->base -= d;
                }
            }
            // ESP_LOGI("UI","Decremented base %u", data->base);
            break;
        case SID_WIDTH:
            {
                int d = accel_step();
                if((data->width - d) <= 0){       //Dummy width value 0 - 255
                    data->width = 0;
                }else{
                    data->width -= d;
                }
            }
            // ESP_LOGI("UI","Decremented width %u", data->width);
            break;
        case SID_Q:
            if((data->q - 1) <= 0){           
                data->q = 0;
            }else{
                data->q -= 1;
            }
            //ESP_LOGI("UI","Decremented q %u", data->q);
            break;    
        default:
            break;
    }
}

void s2_incPlaymodeValue(play_state_data_t* data, int index, xQueueHandle mode_handle){
    switch (index)
    {
        case SID_MODE:
            switch(data->mode){
                case SINGLE:
                    data->mode = LOOP;
                    break;
                case LOOP:
                    data->mode = PIPO;
                    break;
                case PIPO:
                    data->mode = CROP;
                    data->loop_start = data->start;
                    data->loop_position = data->start;
                    break;
                case CROP:
                    data->mode = SINGLE;
                    data->loop_start = data->start;
                    data->loop_position = data->start;
                    break;
            }
            // ESP_LOGI("UI","Changed mode %d", data->mode);
            break;
        case SID_START:
            if(data->start < data->loop_end - 8){
                int lim = data->loop_end - 8;
                if(lim > 800) lim = 800;
                int d = accel_step();
                if(data->start + d > lim) d = lim - data->start;
                data->start += d;

                if(data->mode == SINGLE || data->mode == CROP){
                    //ESP_LOGI("UI", "DATA MODE SINGLE");
                    data->loop_start = data->start;
                    data->loop_position = data->start;
                }
            }

            // ESP_LOGI("UI","Incremented start %u", data->start);
            break;
        case SID_LSTART:
            if(data->loop_start < data->loop_end - 8){
                int lim = data->loop_end - 8;
                if(lim > 800) lim = 800;
                int d = accel_step();
                if(data->loop_start + d > lim) d = lim - data->loop_start;
                data->loop_start += d;
                data->loop_position += d;
                data->loop_length -= d;
                if(data->mode == SINGLE || data->mode == CROP) data->start = data->loop_start;
            }

            // ESP_LOGI("UI","Incremented loop_start %u", data->loop_start);
            break;
        case SID_LEND:
            {
                int d = accel_step();
                if(data->loop_end + d > 800) d = 800 - data->loop_end;
                data->loop_end += d;
                data->loop_length += d;
            }
            // ESP_LOGI("UI","Incremented loop_end %u", data->loop_end);
            break;
        case SID_LPOSITION:
            if(data->loop_end != 800){
                // slide the whole loop window up; clamp so the end stops at 800
                int d = accel_step();
                if(data->loop_end + d > 800) d = 800 - data->loop_end;
                data->loop_position += d;
                data->loop_start = data->loop_position;
                if(data->mode == SINGLE || data->mode == CROP){
                    data->start = data->loop_position;
                }
                data->loop_end += d;
                if(data->loop_end == 800)
                    data->loop_length = data->loop_end - data->loop_start;
            }
            break;
        default:
            break;
    }
    // xQueueSend(mode_handle, data, 0);
}

void s2_decPlaymodeValue(play_state_data_t* data, int index, xQueueHandle mode_handle){
    switch (index)
    {
        case SID_MODE:
            switch(data->mode){
                case SINGLE:
                    data->mode = CROP;
                    data->loop_start = data->start;
                    data->loop_position = data->start;
                    break;
                case CROP:
                    data->mode = PIPO;
                    break;
                case LOOP:
                    data->mode = SINGLE;
                    data->loop_start = data->start;
                    data->loop_position = data->start;
                    break;
                case PIPO:
                    data->mode = LOOP;
                    break;
            }
            
            // ESP_LOGI("UI","Changed mode %d", data->mode);
            break;
        case SID_START:
            {
                int d = accel_step();
                if(d > data->start) d = data->start;
                data->start -= d;
            }
            if(data->mode == SINGLE || data->mode == CROP){
                data->loop_start = data->start;
                data->loop_position = data->start;
            }
            // ESP_LOGI("UI","Decremented start %u", data->start);
            break;
        case SID_LSTART:
            {
                int d = accel_step();
                if(d > data->loop_start) d = data->loop_start;
                data->loop_start -= d;
                data->loop_position -= d;
                data->loop_length += d;
                if(data->loop_start == 0) data->loop_position = 0;
            }
            if(data->mode == SINGLE || data->mode == CROP){
                data->start = data->loop_start;
                data->loop_position = data->loop_start;
            }
            //ESP_LOGI("UI","Looplength %u", data->loop_length);
            break;
        case SID_LEND:
            if(data->loop_end > data->loop_start + 8 && data->loop_end > data->start + 8){
                // keep loop_end at least 8 above both start marks
                int floor_ = ((data->loop_start > data->start) ? data->loop_start : data->start) + 8;
                int d = accel_step();
                if(data->loop_end - d < floor_) d = data->loop_end - floor_;
                data->loop_end -= d;
                data->loop_length -= d;
            }

            // ESP_LOGI("UI","Decremented loop_end %u", data->loop_end);
            break;
        case SID_LPOSITION:
            //Loop Position
            if(data->loop_start != 0 && data->loop_position != 0 && data->loop_end > data->start+ 8){
                // slide the whole loop window down; clamp at position 0 and
                // keep loop_end at least 8 above start
                int d = accel_step();
                if(d > data->loop_position) d = data->loop_position;
                if(data->loop_end - d < data->start + 8) d = data->loop_end - (data->start + 8);
                data->loop_position -= d;
                data->loop_start = data->loop_position;
                if(data->mode == SINGLE || data->mode == CROP){
                    data->start = data->loop_position;
                }
                data->loop_end -= d;
            }

            break;
        default:
            break;
    }
    // xQueueSend(mode_handle, data, 0);
}

void s2_incItemAmount(matrix_ui_row_t* matrix, int source){
    if(matrix[source].dst != MTX_NONE){
        uint8_t* tmp = &(matrix[source].amt);
        if((*tmp  + 1) >= 100){
            *tmp  = 100;
        }else{
            (*tmp)++;
        }
        //ESP_LOGI("UI", "Incr Source: %d, Amount: %d, Param: %d", source, *tmp , matrix[source].dst);
    }

}

void s2_decItemAmount(matrix_ui_row_t* matrix, int source){
    if(matrix[source].dst != MTX_NONE){
        uint8_t* tmp = &(matrix[source].amt);
        if((*tmp - 1) <= 0){
            *tmp = 0;
        } else{
            (*tmp)--;
        }
       //ESP_LOGI("UI", "Decr Source: %d, Amount: %d , Param: %d", source, *tmp , matrix[source].dst);
    }

}

void s2_incDestination(matrix_ui_row_t* matrix, int source){
    int j = 1;
    // Sampler2: rows 0/1 are reassignable like any other row
    
    for(int i = 0; i < 8; i++){
        if(source == i) continue;

        if(matrix[source].dst + j == matrix[i].dst){
            j++;
            i = -1;
        }  
    }
    if(matrix[source].dst + j >= 37) return;
    matrix[source].dst += j;
    //ESP_LOGI("UI", "Incr - Source: %d, Amount: %d , Dest: %d", source, matrix[source].amt, matrix[source].dst);
}

void s2_decDestination(matrix_ui_row_t* matrix, int source){
    int j = 1;
    // Sampler2: rows 0/1 are reassignable like any other row

    for(int i = 0; i < 8; i++){
        if(source == i) continue;
        if(matrix[source].dst - j <= 0 || matrix[source].dst - j > 37){
            matrix[source].dst = MTX_NONE;
            return;
        } 
        if(matrix[source].dst - j == matrix[i].dst){
            j++;
            i = -1;
        }  
    }
    matrix[source].dst -= j;
    //ESP_LOGI("UI", "Decr - Source: %d, Amount: %d , Dest: %d", source, matrix[source].amt, matrix[source].dst);
}

void s2_incDelayValue(delay_data_t* delay, int index){
    switch(index){
        case SID_DELAY_ACTIVE:
            delay->is_active = !(delay->is_active);
            // ESP_LOGI("UI","Changed is_active %d", delay->is_active);
            break;
        case SID_DELAY_MODE:
            delay->mode = !(delay->mode);
            break;
        case SID_DELAY_TIME:
            {
                int d = 2 * accel_step();
                if((delay->time + d) >= 1500){
                    delay->time = 1500;
                }else{
                    delay->time += d;
                }
            }
            // ESP_LOGI("UI","Incremented delay_time_left %u", delay->delay_time_left);
            break;
        case SID_DELAY_PAN:
            if((delay->pan + 1) >= 128){   //Q1.7 128 = 1.0
                delay->pan = 128;
            }else{
                delay->pan += 1;
            }
            // ESP_LOGI("UI","Incremented delay_time_right %u", delay->delay_time_right);
            break;
        case SID_DELAY_FEEDBACK:
            if((delay->feedback + 1) >= 100){   
                delay->feedback = 100;
            }else{
                delay->feedback += 1;
            }
            // ESP_LOGI("UI","Incremented feedback %u", delay->feedback);
            break;
        case SID_DELAY_VOLUME:
            if((delay->volume + 1) >= 200){   //Q2.6 128 = 2.0
                delay->volume = 200;
            }else{
                delay->volume += 1;
            }
            // ESP_LOGI("UI","Incremented delay_volume %u", delay->delay_volume);
            break;
    }
}

void s2_decDelayValue(delay_data_t* delay, int index){
        switch(index){
        case SID_DELAY_ACTIVE:
            delay->is_active = !(delay->is_active);
            // ESP_LOGI("UI","Changed is_active %d", delay->is_active);
            break;
        case SID_DELAY_MODE:
            delay->mode = !(delay->mode);
            break;
        case SID_DELAY_TIME:
            {
                int d = 2 * accel_step();
                if((delay->time - d) < 2){
                    delay->time = 2;
                }else{
                    delay->time -= d;
                }
            }
            // ESP_LOGI("UI","Decremented delay_time_left %u", delay->delay_time_left);
            break;
        case SID_DELAY_PAN:
            if((delay->pan - 1) < 0){
                delay->pan = 0;
            }else{
                delay->pan -= 1;   
            }
            // ESP_LOGI("UI","Decremented delay_time_right %u", delay->delay_time_right);
            break; 
        case SID_DELAY_FEEDBACK:
            if((delay->feedback - 1) < 0){
                delay->feedback = 0;
            }else{
                delay->feedback -= 1;   
            }
            // ESP_LOGI("UI","Decremented feedback %u", delay->feedback);
            break;
        case SID_DELAY_VOLUME:
            if((delay->volume - 1) < 0){
                delay->volume = 0;
            }else{
                delay->volume -= 1;
            }
            // ESP_LOGI("UI","Decremented delay_volume %u", delay->delay_volume);
            break;
    }
}

void s2_incExtInValue(ext_in_data_t* extin, int index){
    switch(index){
        case SID_EXTIN_ACTIVE:
            extin->is_active = !(extin->is_active);
            break;
        case SID_EXTIN_PAN:
            if((extin->pan + 128) >= 16384){ // 16384 is +1.0
                extin->pan = 16384;
            }else{
                extin->pan += 128; 
            }
            break;
        case SID_EXTIN_DELAY_SEND:
            if((extin->delay_send + 1) >= 100){   
                extin->delay_send = 100;
            }else{
                extin->delay_send += 1;
            }
            break;
        case SID_EXTIN_VOLUME:
            if((extin->volume + 1) >= 200){   //Q2.6 128 = 2.0
                extin->volume = 200;
            }else{
                extin->volume += 1;
            }
            break;
    }
}


void s2_decExtInValue(ext_in_data_t* extin, int index){
        switch(index){
        case SID_EXTIN_ACTIVE:
            extin->is_active = !(extin->is_active);
            break;
        case SID_EXTIN_PAN:
            if((extin->pan - 128) <= -16384){ // -16384 is -1.0
                extin->pan = -16384;
            }else{
                extin->pan -= 128;
            }
            break; 
        case SID_EXTIN_DELAY_SEND:
            if((extin->delay_send - 1) < 0){
                extin->delay_send = 0;
            }else{
                extin->delay_send -= 1;   
            }
            break;
        case SID_EXTIN_VOLUME:
            if((extin->volume - 1) < 0){
                extin->volume = 0;
            }else{
                extin->volume -= 1;
            }
            break;
    }
}

