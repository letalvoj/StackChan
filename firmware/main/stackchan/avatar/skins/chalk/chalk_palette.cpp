/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "chalk_palette.h"

namespace stackchan::avatar::chalk {

// Openness ladders come from the generator, which ranks each family's frames by how much
// mouth they actually show. They used to be written here from a reading of the artist's
// pose names, and that reading was wrong: it ranked the grumpy "tight teeth" pose mid-way
// when it is near the widest, so an angry face never opened as far as it was drawn to.

static EmotionClips makeNeutral()
{
    EmotionClips e;
    e.eyesLeft       = &clip_eyes_blink_left;
    e.eyesRight      = &clip_eyes_blink_right;
    e.eyesRestFrame  = 0;                        // frame 0 of a blink is the open eye
    e.mouthRest      = &clip_mouth_quiet_smile;
    e.mouthRestFrame = 0;
    e.mouthSpeech    = &clip_mouth_speech_neutral;
    e.mouthIdle      = &clip_mouth_quiet_smile;  // "occasional, not continuous"
    e.cheeksLeft     = &clip_cheeks_smile_lift_left;
    e.cheeksRight    = &clip_cheeks_smile_lift_right;
    e.cheeksRestFrame = 0;                       // frame 0 is the cheek at rest
    e.gazeLimit      = kGazeLimit_blink;
    e.openness       = clip_mouth_speech_neutral_openness;
    e.opennessCount  = clip_mouth_speech_neutral_opennessCount;
    return e;
}

static EmotionClips makeHappy()
{
    EmotionClips e   = makeNeutral();
    e.eyesLeft       = &clip_eyes_smile_squeeze_left;
    e.eyesRight      = &clip_eyes_smile_squeeze_right;
    e.eyesRestFrame  = 3;                        // the squeeze itself, not the approach
    e.mouthRest      = &clip_mouth_speech_delighted;
    e.mouthRestFrame = 0;                        // the family's own wide smiling rest
    e.mouthSpeech    = &clip_mouth_speech_delighted;
    e.mouthIdle      = nullptr;                  // a delighted rest is a held wide smile;
                                                 // the only family it could fidget with is
                                                 // its own speech row
    // No brows. A squeezed happy eye is already an arc, and putting the brow arc above it
    // draws the same shape twice -- the reference's delighted face has one arc per eye and
    // nothing over it.
    e.browsLeft      = nullptr;
    e.browsRight     = nullptr;
    e.cheeksLeft     = &clip_cheeks_smile_lift_left;
    e.cheeksRight    = &clip_cheeks_smile_lift_right;
    e.cheeksRestFrame = 3;                       // lifted
    e.gazeLimit      = kGazeLimit_smile_squeeze;
    e.openness       = clip_mouth_speech_delighted_openness;
    e.opennessCount  = clip_mouth_speech_delighted_opennessCount;
    return e;
}

static EmotionClips makeAngry()
{
    EmotionClips e   = makeNeutral();
    e.eyesLeft       = &clip_eyes_grumpy_squint_left;
    e.eyesRight      = &clip_eyes_grumpy_squint_right;
    e.eyesRestFrame  = 1;                        // narrowed but still open, pupils showing:
                                                 // the glare is the pupil under a heavy lid
    e.mouthRest      = &clip_mouth_speech_grumpy;
    e.mouthRestFrame = 0;                        // the pressed rest of the grumpy family
    e.mouthSpeech    = &clip_mouth_speech_grumpy;
    e.mouthIdle      = nullptr;                  // a held, pressed line. A grumpy mouth
                                                 // moving on its own would be sulking aloud
    e.browsLeft      = &clip_brows_stubborn_left;
    e.browsRight     = &clip_brows_stubborn_right;
    e.browsRestFrame = 3;
    // The puffed bracket is not decoration: in the reference it is the thing that makes a
    // grumpy face read as grumpy, framing a mouth that is otherwise just a pressed line.
    e.cheeksLeft     = &clip_cheeks_puffed_hmph_left;
    e.cheeksRight    = &clip_cheeks_puffed_hmph_right;
    e.cheeksRestFrame = 3;
    e.gazeLimit      = kGazeLimit_grumpy_squint;
    e.openness       = clip_mouth_speech_grumpy_openness;
    e.opennessCount  = clip_mouth_speech_grumpy_opennessCount;
    return e;
}

static EmotionClips makeSad()
{
    EmotionClips e   = makeNeutral();
    e.eyesLeft       = &clip_eyes_heavy_sad_left;
    e.eyesRight      = &clip_eyes_heavy_sad_right;
    e.eyesRestFrame  = 1;                        // heavy but not shut; the pupils carry it
    e.mouthRest      = &clip_mouth_speech_sad;
    e.mouthRestFrame = 0;
    e.mouthSpeech    = &clip_mouth_speech_sad;
    e.mouthIdle      = nullptr;                  // likewise: the pack draws no quiet sad
                                                 // mouth, only a speaking one
    e.browsLeft      = &clip_brows_concern_left;
    e.browsRight     = &clip_brows_concern_right;
    e.browsRestFrame = 3;
    e.cheeksRestFrame = 0;
    e.gazeLimit      = kGazeLimit_heavy_sad;
    e.openness       = clip_mouth_speech_sad_openness;
    e.opennessCount  = clip_mouth_speech_sad_opennessCount;
    return e;
}

static EmotionClips makeDoubt()
{
    EmotionClips e   = makeNeutral();
    e.eyesLeft       = &clip_eyes_curious_widen_left;
    e.eyesRight      = &clip_eyes_curious_widen_right;
    e.eyesRestFrame  = 2;                        // left eye normal, right widened, both
                                                 // pupils showing. The asymmetry is the
                                                 // whole expression: composed-expressions.png
                                                 // draws "what was that?" exactly this way,
                                                 // and frames 3-4 are the ones to avoid --
                                                 // they drop the right pupil entirely.
    e.mouthRest      = &clip_mouth_thinking;
    e.mouthRestFrame = 2;
    e.mouthSpeech    = &clip_mouth_speech_neutral;
    e.mouthIdle      = &clip_mouth_thinking;     // "one curious reaction" -- exactly the
                                                 // sort of thing a quiet mouth does
    e.browsLeft      = &clip_brows_curiosity_left;
    e.browsRight     = &clip_brows_curiosity_right;
    e.browsRestFrame = 2;                        // one brow high, the other flat. That *is*
                                                 // curiosity in this pack -- a matched pair
                                                 // of arches reads as mild surprise -- and it
                                                 // pairs with the widened right eye above.
    e.cheeksRestFrame = 0;
    e.gazeLimit      = kGazeLimit_curious_widen;
    e.openness       = clip_mouth_speech_neutral_openness;
    e.opennessCount  = clip_mouth_speech_neutral_opennessCount;
    return e;
}

static EmotionClips makeSleepy()
{
    EmotionClips e   = makeNeutral();
    e.eyesLeft       = &clip_eyes_sleepy_drift_left;
    e.eyesRight      = &clip_eyes_sleepy_drift_right;
    e.eyesRestFrame  = 2;                        // the longest-held, most closed frame
    e.mouthRest      = &clip_mouth_yawn;
    e.mouthRestFrame = 0;
    e.mouthSpeech    = &clip_mouth_yawn;
    e.mouthIdle      = &clip_mouth_yawn;         // the pack calls the yawn an "event", and a
                                                 // sleepy face yawning unprompted is the point
    e.browsLeft      = nullptr;                  // the reference draws sleepy bare
    e.browsRight     = nullptr;
    e.cheeksLeft     = nullptr;
    e.cheeksRight    = nullptr;
    e.gazeLimit      = kGazeLimit_sleepy_drift;
    e.openness       = clip_mouth_yawn_openness;
    e.opennessCount  = clip_mouth_yawn_opennessCount;
    return e;
}

static EmotionClips makeLaugh()
{
    // Almost everything here is led by the mouth. Each laugh mouth frame carries the eyes,
    // cheeks and face lift the artist paired with it (see ClipCompanion), so the values set
    // below are only what shows before the first mouth frame is placed -- and they are the
    // grin's own pairing, so there is no visible hand-over.
    EmotionClips e   = makeHappy();
    e.eyesLeft       = &clip_eyes_laugh_left;
    e.eyesRight      = &clip_eyes_laugh_right;
    e.eyesRestFrame  = 0;                        // the soft squeeze the grin wears
    e.mouthRest      = &clip_mouth_laugh;
    e.mouthRestFrame = 0;                        // grin
    // Talking while laughing walks the six laugh drawings by amplitude, so a loud syllable
    // lands on "haa" and brings the > < eyes with it.
    e.mouthSpeech    = &clip_mouth_laugh;
    // Silent, the face performs the artist's burst: two uneven bursts and a recovery.
    e.mouthIdle      = &clip_mouth_laugh_burst;
    e.mouthIdleOnEnter = true;
    e.cheeksRestFrame = 2;
    e.openness       = clip_mouth_laugh_openness;
    e.opennessCount  = clip_mouth_laugh_opennessCount;
    return e;
}

const EmotionClips& clipsFor(Emotion emotion)
{
    static const EmotionClips kNeutral = makeNeutral();
    static const EmotionClips kHappy   = makeHappy();
    static const EmotionClips kAngry   = makeAngry();
    static const EmotionClips kSad     = makeSad();
    static const EmotionClips kDoubt   = makeDoubt();
    static const EmotionClips kSleepy  = makeSleepy();
    static const EmotionClips kLaugh   = makeLaugh();

    switch (emotion) {
        case Emotion::Happy:  return kHappy;
        case Emotion::Angry:  return kAngry;
        case Emotion::Sad:    return kSad;
        case Emotion::Doubt:  return kDoubt;
        case Emotion::Sleepy: return kSleepy;
        case Emotion::Laugh:  return kLaugh;
        case Emotion::Neutral:
        default:              return kNeutral;
    }
}

}  // namespace stackchan::avatar::chalk
