/**********************************************************************

  Audacity: A Digital Audio Editor

  Loudness.cpp

  Max Maisel

*******************************************************************//**

\class EffectLoudness
\brief An Effect to bring the loudness level up to a chosen level.

*//*******************************************************************/


#include "../Audacity.h" // for rint from configwin.h
#include "Loudness.h"

#include <math.h>

#include <wx/intl.h>
#include <wx/valgen.h>

#include "../Internat.h"
#include "../Prefs.h"
#include "../Shuttle.h"
#include "../ShuttleGui.h"
#include "../WaveTrack.h"
#include "../widgets/valnum.h"

enum kNormalizeTargets
{
   kLoudness,
   kRMS,
   nAlgos
};

static const ComponentInterfaceSymbol kNormalizeTargetStrings[nAlgos] =
{
   { XO("perceived loudness") },
   { XO("RMS") }
};
// Define keys, defaults, minimums, and maximums for the effect parameters
//
//     Name         Type     Key                        Def         Min      Max       Scale
Param( StereoInd,   bool,    wxT("StereoIndependent"),   false,      false,   true,     1  );
Param( LUFSLevel,   double,  wxT("LUFSLevel"),           -23.0,      -145.0,  0.0,      1  );
Param( RMSLevel,    double,  wxT("RMSLevel"),            -20.0,      -145.0,  0.0,      1  );
Param( DualMono,    bool,    wxT("DualMono"),            true,       false,   true,     1  );
Param( NormalizeTo, int,     wxT("NormalizeTo"),         kLoudness , 0    ,   nAlgos-1, 1  );

BEGIN_EVENT_TABLE(EffectLoudness, wxEvtHandler)
   EVT_CHOICE(wxID_ANY, EffectLoudness::OnUpdateUI)
   EVT_CHECKBOX(wxID_ANY, EffectLoudness::OnUpdateUI)
   EVT_TEXT(wxID_ANY, EffectLoudness::OnUpdateUI)
END_EVENT_TABLE()

EffectLoudness::EffectLoudness()
{
   mStereoInd = DEF_StereoInd;
   mLUFSLevel = DEF_LUFSLevel;
   mRMSLevel = DEF_RMSLevel;
   mDualMono = DEF_DualMono;
   mNormalizeTo = DEF_NormalizeTo;

   SetLinearEffectFlag(false);
}

EffectLoudness::~EffectLoudness()
{
}

// ComponentInterface implementation

ComponentInterfaceSymbol EffectLoudness::GetSymbol()
{
   return LOUDNESS_PLUGIN_SYMBOL;
}

wxString EffectLoudness::GetDescription()
{
   return _("Sets the loudness of one or more tracks");
}

wxString EffectLoudness::ManualPage()
{
   return wxT("Loudness");
}

// EffectDefinitionInterface implementation

EffectType EffectLoudness::GetType()
{
   return EffectTypeProcess;
}

// EffectClientInterface implementation
bool EffectLoudness::DefineParams( ShuttleParams & S )
{
   S.SHUTTLE_PARAM( mStereoInd, StereoInd );
   S.SHUTTLE_PARAM( mLUFSLevel, LUFSLevel );
   S.SHUTTLE_PARAM( mRMSLevel, RMSLevel );
   S.SHUTTLE_PARAM( mDualMono, DualMono );
   S.SHUTTLE_PARAM( mNormalizeTo, NormalizeTo );
   return true;
}

bool EffectLoudness::GetAutomationParameters(CommandParameters & parms)
{
   parms.Write(KEY_StereoInd, mStereoInd);
   parms.Write(KEY_LUFSLevel, mLUFSLevel);
   parms.Write(KEY_RMSLevel, mRMSLevel);
   parms.Write(KEY_DualMono, mDualMono);
   parms.Write(KEY_NormalizeTo, mNormalizeTo);

   return true;
}

bool EffectLoudness::SetAutomationParameters(CommandParameters & parms)
{
   ReadAndVerifyBool(StereoInd);
   ReadAndVerifyDouble(LUFSLevel);
   ReadAndVerifyDouble(RMSLevel);
   ReadAndVerifyBool(DualMono);
   ReadAndVerifyBool(NormalizeTo);

   mStereoInd = StereoInd;
   mLUFSLevel = LUFSLevel;
   mRMSLevel = RMSLevel;
   mDualMono = DualMono;
   mNormalizeTo = NormalizeTo;

   return true;
}

// Effect implementation

bool EffectLoudness::CheckWhetherSkipEffect()
{
   return false;
}

bool EffectLoudness::Startup()
{
   wxString base = wxT("/Effects/Loudness/");
   // Load the old "current" settings
   if (gPrefs->Exists(base))
   {
      mStereoInd = true;
      mDualMono = DEF_DualMono;
      mNormalizeTo = kLoudness;
      mLUFSLevel = DEF_LUFSLevel;
      mRMSLevel = DEF_RMSLevel;

      SaveUserPreset(GetCurrentSettingsGroup());

      gPrefs->Flush();
   }
   return true;
}

// TODO: more method extraction
bool EffectLoudness::Process()
{
   if(mNormalizeTo == kLoudness)
      // LU use 10*log10(...) instead of 20*log10(...)
      // so multiply level by 2 and use standard DB_TO_LINEAR macro.
      mRatio = DB_TO_LINEAR(TrapDouble(mLUFSLevel*2, MIN_LUFSLevel, MAX_LUFSLevel));

   // Iterate over each track
   this->CopyInputTracks(); // Set up mOutputTracks.
   bool bGoodResult = true;
   wxString topMsg = _("Normalizing Loudness...\n");

   AllocBuffers();
   mProgressVal = 0;

   for(auto track : mOutputTracks->Selected<WaveTrack>()
       + (mStereoInd ? &Track::Any : &Track::IsLeader))
   {
      // Get start and end times from track
      // PRL: No accounting for multiple channels ?
      double trackStart = track->GetStartTime();
      double trackEnd = track->GetEndTime();

      // Set the current bounds to whichever left marker is
      // greater and whichever right marker is less:
      mCurT0 = mT0 < trackStart? trackStart: mT0;
      mCurT1 = mT1 > trackEnd? trackEnd: mT1;

      // Get the track rate
      mCurRate = track->GetRate();

      wxString msg;
      auto trackName = track->GetName();
      mSteps = 2;

      mProgressMsg =
         topMsg + wxString::Format(_("Analyzing: %s"), trackName);

      auto range = mStereoInd
         ? TrackList::SingletonRange(track)
         : TrackList::Channels(track);

      mProcStereo = range.size() > 1;

      InitTrackAnalysis();
      if(!ProcessOne(range, true))
      {
         // Processing failed -> abort
         bGoodResult = false;
         break;
      }

      // Calculate normalization values the analysis results
      float extent = 1.0;
      // TODO: add it in separate method
      mMult = mRatio / extent;

      mProgressMsg = topMsg + wxString::Format(_("Processing: %s"), trackName);
      if(!ProcessOne(range, false))
      {
         // Processing failed -> abort
         bGoodResult = false;
         break;
      }
   }

   this->ReplaceProcessedTracks(bGoodResult);
   FreeBuffers();
   return bGoodResult;
}

void EffectLoudness::PopulateOrExchange(ShuttleGui & S)
{
   S.StartVerticalLay(0);
   {
      S.StartMultiColumn(2, wxALIGN_CENTER);
      {
         S.StartVerticalLay(false);
         {
            S.StartHorizontalLay(wxALIGN_LEFT, false);
            {
               S.AddVariableText(_("Normalize"), false,
                                 wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);

               auto targetChoices = LocalizedStrings(kNormalizeTargetStrings, nAlgos);
               mNormalizeToCtl = S.AddChoice(wxEmptyString, targetChoices, mNormalizeTo);
               mNormalizeToCtl->SetValidator(wxGenericValidator(&mNormalizeTo));
               S.AddVariableText(_("to"), false,
                                 wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);

               FloatingPointValidator<double> vldLevel(2, &mLUFSLevel,
                                                       NumValidatorStyle::ONE_TRAILING_ZERO);
               vldLevel.SetRange( MIN_LUFSLevel, MAX_LUFSLevel);

               mLevelTextCtrl = S.AddTextBox( {}, wxT(""), 10);
               /* i18n-hint: LUFS is a particular method for measuring loudnesss */
               mLevelTextCtrl->SetName( _("Loudness LUFS"));
               mLevelTextCtrl->SetValidator(vldLevel);
               /* i18n-hint: LUFS is a particular method for measuring loudnesss */
               mLeveldB = S.AddVariableText(_("LUFS"), false,
                                            wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);
               mWarning = S.AddVariableText( {}, false,
                                            wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT);
            }
            S.EndHorizontalLay();

            mStereoIndCheckBox = S.AddCheckBox(_("Normalize stereo channels independently"),
                                               mStereoInd ? wxT("true") : wxT("false"));
            mStereoIndCheckBox->SetValidator(wxGenericValidator(&mStereoInd));

            mDualMonoCheckBox = S.AddCheckBox(_("Treat mono as dual-mono (recommended)"),
                                              mDualMono ? wxT("true") : wxT("false"));
            mDualMonoCheckBox->SetValidator(wxGenericValidator(&mDualMono));
         }
         S.EndVerticalLay();
      }
      S.EndMultiColumn();
   }
   S.EndVerticalLay();
   // To ensure that the UpdateUI on creation sets the prompts correctly.
   mGUINormalizeTo = !mNormalizeTo;
}

bool EffectLoudness::TransferDataToWindow()
{
   if (!mUIParent->TransferDataToWindow())
   {
      return false;
   }

   UpdateUI();
   return true;
}

bool EffectLoudness::TransferDataFromWindow()
{
   if (!mUIParent->Validate() || !mUIParent->TransferDataFromWindow())
   {
      return false;
   }
   return true;
}

// EffectLoudness implementation

/// Get required buffer size for the largest whole track and allocate buffers.
/// This reduces the amount of allocations required.
void EffectLoudness::AllocBuffers()
{
   mTrackBufferCapacity = 0;
   bool stereoTrackFound = false;
   double maxSampleRate = 0;
   mProcStereo = false;

   for(auto track : mOutputTracks->Selected<WaveTrack>() + &Track::Any)
   {
      mTrackBufferCapacity = std::max(mTrackBufferCapacity, track->GetMaxBlockSize());
      maxSampleRate = std::max(maxSampleRate, track->GetRate());

      // There is a stereo track
      if(track->IsLeader())
         stereoTrackFound = true;
   }

   // TODO: hist and block buffers go here

   // Initiate a processing buffer.  This buffer will (most likely)
   // be shorter than the length of the track being processed.
   mTrackBuffer[0].reinit(mTrackBufferCapacity);

   if(!mStereoInd && stereoTrackFound)
      mTrackBuffer[1].reinit(mTrackBufferCapacity);
}

void EffectLoudness::FreeBuffers()
{
   mTrackBuffer[0].reset();
   mTrackBuffer[1].reset();
   // TODO: additional destroy -> function
}

void EffectLoudness::InitTrackAnalysis()
{
   mCount = 0;
  // TODO: additional init goes here
}

/// ProcessOne() takes a track, transforms it to bunch of buffer-blocks,
/// and executes ProcessData, on it...
///  uses mMult to normalize a track.
///  mMult must be set before this is called
/// In analyse mode, it executes the selected analyse operation on it...
///  mMult does not have to be set before this is called
bool EffectLoudness::ProcessOne(TrackIterRange<WaveTrack> range, bool analyse)
{
   WaveTrack* track = *range.begin();

   // Transform the marker timepoints to samples
   auto start = track->TimeToLongSamples(mCurT0);
   auto end   = track->TimeToLongSamples(mCurT1);

   // Get the length of the buffer (as double). len is
   // used simply to calculate a progress meter, so it is easier
   // to make it a double now than it is to do it later
   mTrackLen = (end - start).as_double();

   // Abort if the right marker is not to the right of the left marker
   if(mCurT1 <= mCurT0)
      return false;

   // Go through the track one buffer at a time. s counts which
   // sample the current buffer starts at.
   auto s = start;
   while(s < end)
   {
      // Get a block of samples (smaller than the size of the buffer)
      // Adjust the block size if it is the final block in the track
      auto blockLen = limitSampleBufferSize(
         track->GetBestBlockSize(s),
         mTrackBufferCapacity);

      const size_t remainingLen = (end - s).as_size_t();
      blockLen = blockLen > remainingLen ? remainingLen : blockLen;
      if(!LoadBufferBlock(range, s, blockLen))
         return false;

      // Process the buffer.
      if(analyse)
      {
         if(!AnalyseBufferBlock())
            return false;
      }
      else
      {
         if(!ProcessBufferBlock())
            return false;
      }

      sleep(1);

      if(!analyse)
         StoreBufferBlock(range, s, blockLen);

      // Increment s one blockfull of samples
      s += blockLen;
   }

   // Return true because the effect processing succeeded ... unless cancelled
   return true;
}

bool EffectLoudness::LoadBufferBlock(TrackIterRange<WaveTrack> range,
                                     sampleCount pos, size_t len)
{
   sampleCount read_size = -1;
   // Get the samples from the track and put them in the buffer
   int idx = 0;
   for(auto channel : range)
   {
      channel->Get((samplePtr) mTrackBuffer[idx].get(), floatSample, pos, len,
                   fillZero, true, &read_size);
      mTrackBufferLen = read_size.as_size_t();

      // Fail if we read different sample count from stereo pair tracks.
      // Ignore this check during first iteration (read_size == -1).
      if(read_size.as_size_t() != mTrackBufferLen && read_size != -1)
         return false;

      ++idx;
   }
   return true;
}

/// Calculates sample sum (for DC) and EBU R128 weighted square sum
/// (for loudness).
bool EffectLoudness::AnalyseBufferBlock()
{
   // TODO: analysis loop goes here
   mCount += mTrackBufferLen;

   if(!UpdateProgress())
      return false;
   return true;
}

bool EffectLoudness::ProcessBufferBlock()
{
   for(size_t i = 0; i < mTrackBufferLen; i++)
   {
      mTrackBuffer[0][i] = mTrackBuffer[0][i] * mMult;
      if(mProcStereo)
         mTrackBuffer[1][i] = mTrackBuffer[1][i] * mMult;
   }

   if(!UpdateProgress())
      return false;
   return true;
}

void EffectLoudness::StoreBufferBlock(TrackIterRange<WaveTrack> range,
                                      sampleCount pos, size_t len)
{
   int idx = 0;
   for(auto channel : range)
   {
      // Copy the newly-changed samples back onto the track.
      channel->Set((samplePtr) mTrackBuffer[idx].get(), floatSample, pos, len);
      ++idx;
   }
}

bool EffectLoudness::UpdateProgress()
{
   mProgressVal += (double(1+mProcStereo) * double(mTrackBufferLen)
                 / (double(GetNumWaveTracks()) * double(mSteps) * mTrackLen));
   return !TotalProgress(mProgressVal, mProgressMsg);
}

void EffectLoudness::OnUpdateUI(wxCommandEvent & WXUNUSED(evt))
{
   UpdateUI();
}

void EffectLoudness::UpdateUI()
{
   if (!mUIParent->TransferDataFromWindow())
   {
      mWarning->SetLabel(_("(Maximum 0dB)"));
      // TODO: recalculate layout here
      EnableApply(false);
      return;
   }
   mWarning->SetLabel(wxT(""));
   EnableApply(true);

   // Changing the prompts causes an unwanted UpdateUI event.  
   // This 'guard' stops that becoming an infinite recursion.
   if (mNormalizeTo != mGUINormalizeTo)
   {
      mGUINormalizeTo = mNormalizeTo;
      if(mNormalizeTo == kLoudness)
      {
         FloatingPointValidator<double> vldLevel(2, &mLUFSLevel, NumValidatorStyle::ONE_TRAILING_ZERO);
         vldLevel.SetRange(MIN_LUFSLevel, MAX_LUFSLevel);
         mLevelTextCtrl->SetValidator(vldLevel);
         /* i18n-hint: LUFS is a particular method for measuring loudnesss */
         mLevelTextCtrl->SetName(_("Loudness LUFS"));
         mLevelTextCtrl->SetValue(wxString::FromDouble(mLUFSLevel));
         /* i18n-hint: LUFS is a particular method for measuring loudnesss */
         mLeveldB->SetLabel(_("LUFS"));
      }
      else // RMS
      {
         FloatingPointValidator<double> vldLevel(2, &mRMSLevel, NumValidatorStyle::ONE_TRAILING_ZERO);
         vldLevel.SetRange(MIN_RMSLevel, MAX_RMSLevel);
         mLevelTextCtrl->SetValidator(vldLevel);
         mLevelTextCtrl->SetName(_("RMS dB"));
         mLevelTextCtrl->SetValue(wxString::FromDouble(mRMSLevel));
         mLeveldB->SetLabel(_("dB"));
      }
   }

   mDualMonoCheckBox->Enable(mNormalizeTo == kLoudness);
}
