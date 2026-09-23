#pragma once
#include <cstdint>

class AvatarController {
public:
    AvatarController();
    ~AvatarController();

    void SetStatus(const char* status);
    void SetEmotion(const char* emotion);
    void SetChatMessage(const char* role, const char* content, bool setup_ui_called);
    void ClearChatMessages();
    void CreateIdleMotionModifier();

    void SetIdleMotionLevel(uint8_t level) { idle_motion_level_ = level; }
    uint8_t GetIdleMotionLevel() const { return idle_motion_level_; }
    void SetBlinkModifierId(int id) { blink_modifier_id_ = id; }

private:
    int speaking_modifier_id_           = -1;
    int idle_motion_modifier_id_        = -1;
    int idle_expression_modifier_id_    = -1;
    int blink_modifier_id_              = -1;
    /// The bubble currently shows the sleepy face's Zzz, which this controller wrote and
    /// so must take back -- as opposed to text some host put there, which it must not.
    bool showing_zzz_                   = false;
    uint8_t idle_motion_level_          = 2;
};
