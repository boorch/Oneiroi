#pragma once

#include "Commons.h"
#include "OpenWareLibrary.h"
#include <cmath>

#define MAX_SPRAY_GRAINS 8

struct SprayGrain {
    bool active;
    float phase;        // 0-1 through grain duration
    float startPos;     // Position in looper buffer (normalized 0-1)
    float duration;     // Grain duration in samples
    float pitchShift;   // Pitch multiplier: 0.25x, 0.5x, 1x, 2x, or 4x ONLY
    float amplitude;    // Grain volume
    
    SprayGrain() : active(false), phase(0), startPos(0), duration(1024), pitchShift(1.0f), amplitude(0.5f) {}
};

class GranularSpray {
private:
    PatchCtrls* patchCtrls_;
    PatchState* patchState_;
    
    SprayGrain grains_[MAX_SPRAY_GRAINS];
    float grainTimer_;
    uint32_t nextGrainIndex_;
    
    // Looper buffer access
    FloatArray* looperBuffer_;
    float currentLooperPos_;    // Current normalized position 0-1
    float looperLength_;        // Current looper length
    float looperStart_;         // Current looper start
    
    // Hermann-style envelope (quick attack, slow decay)
    float getHermannEnvelope(float phase) {
        if (phase < 0.1f) {
            // Quick attack: 0 to 1 in first 10%
            return phase * 10.0f;
        } else {
            // Slow decay: 1 to 0 over remaining 90%
            return 1.0f - ((phase - 0.1f) / 0.9f);
        }
    }
    
    // Get one of exactly 5 pitch values based on pitch control
    float getPitchShift(float pitchControl) {
        // Always return exactly one of these 5 values: 0.25x, 0.5x, 1x, 2x, 4x
        
        // Calculate weights for each pitch based on control position
        float weights[5] = {0, 0, 0, 0, 0}; // -2oct, -1oct, normal, +1oct, +2oct
        
        // Normal pitch weight (decreases from 100 at center to 0 at extremes)
        if (pitchControl >= 0.5f) {
            // Upper half: 0.5 to 1.0
            weights[2] = 100.0f * (1.0f - (pitchControl - 0.5f) * 2.0f); // 100 -> 0
        } else {
            // Lower half: 0.5 to 0.0  
            weights[2] = 100.0f * (1.0f - (0.5f - pitchControl) * 2.0f); // 100 -> 0
        }
        
        // 1 octave higher weight
        if (pitchControl >= 0.5f) {
            // 0.5 to 0.75: increases from 0 to 100
            // 0.75 and above: stays at 100
            if (pitchControl <= 0.75f) {
                weights[3] = 100.0f * ((pitchControl - 0.5f) * 4.0f); // 0 -> 100
            } else {
                weights[3] = 100.0f; // stays at 100
            }
        } else {
            weights[3] = 0.0f; // no +1oct in lower half
        }
        
        // 2 octaves higher weight
        if (pitchControl >= 0.75f) {
            // 0.75 to 1.0: increases from 0 to 100
            weights[4] = 100.0f * ((pitchControl - 0.75f) * 4.0f); // 0 -> 100
        } else {
            weights[4] = 0.0f; // no +2oct below 0.75
        }
        
        // 1 octave lower weight
        if (pitchControl <= 0.5f) {
            // 0.5 to 0.25: increases from 0 to 100
            // 0.25 and below: stays at 100
            if (pitchControl >= 0.25f) {
                weights[1] = 100.0f * ((0.5f - pitchControl) * 4.0f); // 0 -> 100
            } else {
                weights[1] = 100.0f; // stays at 100
            }
        } else {
            weights[1] = 0.0f; // no -1oct in upper half
        }
        
        // 2 octaves lower weight
        if (pitchControl <= 0.25f) {
            // 0.25 to 0.0: increases from 0 to 100
            weights[0] = 100.0f * ((0.25f - pitchControl) * 4.0f); // 0 -> 100
        } else {
            weights[0] = 0.0f; // no -2oct above 0.25
        }
        
        // Calculate total weight
        float totalWeight = weights[0] + weights[1] + weights[2] + weights[3] + weights[4];
        
        // Pick randomly based on weights
        if (totalWeight > 0) {
            float random = (rand() / (float)RAND_MAX) * totalWeight;
            float cumulative = 0;
            float pitches[5] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
            
            for (int i = 0; i < 5; i++) {
                cumulative += weights[i];
                if (random <= cumulative) {
                    return pitches[i];
                }
            }
        }
        
        return 1.0f; // Fallback to normal pitch
    }
    
public:
    GranularSpray(PatchCtrls* patchCtrls, PatchState* patchState) 
        : patchCtrls_(patchCtrls), patchState_(patchState),
          grainTimer_(0), nextGrainIndex_(0), looperBuffer_(nullptr),
          currentLooperPos_(0), looperLength_(1), looperStart_(0)
    {
        // Initialize all grains as inactive
        for (int i = 0; i < MAX_SPRAY_GRAINS; i++) {
            grains_[i].active = false;
        }
    }
    
    void SetLooperBuffer(FloatArray* buffer, float position, float length, float start) {
        looperBuffer_ = buffer;
        currentLooperPos_ = position;
        looperLength_ = length;
        looperStart_ = start;
    }
    
    void TriggerGrain() {
        float sprayAmount = patchCtrls_->granularSpray;
        if (sprayAmount <= 0.001f) return; // No grains if spray is zero
        
        // Find inactive grain
        for (int i = 0; i < MAX_SPRAY_GRAINS; i++) {
            int grainIndex = (nextGrainIndex_ + i) % MAX_SPRAY_GRAINS;
            if (!grains_[grainIndex].active) {
                SprayGrain& grain = grains_[grainIndex];
                grain.active = true;
                grain.phase = 0.0f;
                
                // Grain size based ONLY on granularGrainSize control (SHIFT + echo repeats)
                float grainSizeControl = patchCtrls_->granularGrainSize; // 0-1
                float minSize = 0.05f;  // 5% of buffer (larger minimum to prevent pitch artifacts)
                float maxSize = 0.3f;   // 30% of buffer (increased maximum for more variety)
                float grainSizeRatio = minSize + grainSizeControl * (maxSize - minSize);
                
                // Convert to time in seconds, then to samples
                float grainTimeSeconds = grainSizeRatio * looperLength_;
                grain.duration = patchState_->sampleRate * grainTimeSeconds;
                
                // Spray position: sprayAmount controls how far grains can be from current position
                float sprayRange = sprayAmount * 0.4f; // Max 40% of buffer length away from current pos
                float randomOffset = (rand() / (float)RAND_MAX - 0.5f) * 2.0f * sprayRange;
                grain.startPos = currentLooperPos_ + randomOffset;
                
                // Wrap around [0, 1] bounds
                while (grain.startPos < 0) grain.startPos += 1.0f;
                while (grain.startPos >= 1.0f) grain.startPos -= 1.0f;
                
                // Pitch shift: ONLY 5 discrete values, controlled ONLY by granularPitch (SHIFT + varispeed)
                // This is set once when grain is created and NEVER changes during grain lifetime
                grain.pitchShift = getPitchShift(patchCtrls_->granularPitch);
                
                grain.amplitude = 0.7f + (rand() / (float)RAND_MAX) * 0.3f; // 0.7-1.0 volume
                nextGrainIndex_ = (grainIndex + 1) % MAX_SPRAY_GRAINS;
                break;
            }
        }
    }
    
    void Process(AudioBuffer& buffer) {
        float sprayAmount = patchCtrls_->granularSpray;
        if (sprayAmount <= 0.001f || !looperBuffer_) return; // No processing if spray is zero
        
        float dryWet = patchCtrls_->granularDryWet; // 0 = dry only, 1 = wet only
        
        float* left = buffer.getSamples(LEFT_CHANNEL);
        float* right = buffer.getSamples(RIGHT_CHANNEL);
        size_t size = buffer.getSize();
        
        for (size_t i = 0; i < size; i++) {
            // Store original (dry) signal
            float dryLeft = left[i];
            float dryRight = right[i];
            
            // Trigger grains: spray amount controls density (higher = more frequent grains)
            grainTimer_ += 1.0f;
            float grainInterval = patchState_->sampleRate * (0.05f + (1.0f - sprayAmount) * 0.2f); // 50-250ms intervals
            
            if (grainTimer_ >= grainInterval) {
                TriggerGrain();
                grainTimer_ = 0.0f;
            }
            
            float grainLeft = 0.0f, grainRight = 0.0f;
            
            // Process existing grains
            for (int g = 0; g < MAX_SPRAY_GRAINS; g++) {
                SprayGrain& grain = grains_[g];
                if (!grain.active) continue;
                
                // Hermann-style envelope (quick attack, slow decay)
                float envelope = getHermannEnvelope(grain.phase);
                
                // Calculate read position: grain pitch is FIXED and only affects playback speed
                // No continuous calculation - pitch was set once at grain creation
                float readAdvance = grain.phase * grain.pitchShift * 0.05f; // Slow grain playback
                float normalizedRead = grain.startPos + readAdvance;
                
                // Wrap around buffer bounds
                while (normalizedRead < 0) normalizedRead += 1.0f;
                while (normalizedRead >= 1.0f) normalizedRead -= 1.0f;
                
                // Convert to actual buffer position (stereo interleaved)
                size_t bufferSize = looperBuffer_->getSize();
                size_t readIndex = (size_t)(normalizedRead * (bufferSize / 2)) * 2;
                
                if (readIndex < bufferSize - 1) {
                    float sampleLeft = looperBuffer_->getElement(readIndex);
                    float sampleRight = looperBuffer_->getElement(readIndex + 1);
                    
                    grainLeft += sampleLeft * envelope * grain.amplitude;
                    grainRight += sampleRight * envelope * grain.amplitude;
                }
                
                // Advance grain phase
                grain.phase += 1.0f / grain.duration;
                if (grain.phase >= 1.0f) {
                    grain.active = false;
                }
            }
            
            // Dry/wet mixing: dryWet controls the blend between original and granular
            // dryWet = 0: only dry signal
            // dryWet = 1: only wet (granular) signal
            left[i] = dryLeft * (1.0f - dryWet) + (dryLeft + grainLeft * 0.8f) * dryWet; 
            right[i] = dryRight * (1.0f - dryWet) + (dryRight + grainRight * 0.8f) * dryWet;
        }
    }
    
    static GranularSpray* create(PatchCtrls* patchCtrls, PatchState* patchState) {
        return new GranularSpray(patchCtrls, patchState);
    }
    
    static void destroy(GranularSpray* granularSpray) {
        delete granularSpray;
    }
};
