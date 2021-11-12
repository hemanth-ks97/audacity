/**********************************************************************

 Audacity: A Digital Audio Editor

 @file RealtimeEffectState.h

 Paul Licameli split from RealtimeEffectManager.cpp

 *********************************************************************/

#ifndef __AUDACITY_REALTIMEEFFECTSTATE_H__
#define __AUDACITY_REALTIMEEFFECTSTATE_H__

#include <atomic>
#include <vector>
#include <cstddef>

class EffectProcessor;

class RealtimeEffectState
{
public:
   explicit RealtimeEffectState( EffectProcessor &effect );

   EffectProcessor &GetEffect() const { return mEffect; }

   bool Suspend();
   bool Resume() noexcept;
   bool AddTrack(int group, unsigned chans, float rate);
   size_t Process(int group,
      unsigned chans, float **inbuf, float **outbuf, size_t numSamples);
   bool IsActive() const noexcept;

private:
   EffectProcessor &mEffect;

   std::vector<int> mGroupProcessor;
   int mCurrentProcessor;

   std::atomic<int> mSuspendCount{ 1 };    // Effects are initially suspended
};

#endif // __AUDACITY_REALTIMEEFFECTSTATE_H__

