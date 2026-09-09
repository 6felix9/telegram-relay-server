#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <string_view>
#include <nlohmann/json.hpp>

enum class JobType {
    SendText,
    SendPhoto,
    SendVoice,
    SendVideoNote,
    SendSticker,
    Delete
};

struct Job {
    int64_t chat_id{0};
    int64_t message_id{0};
    int64_t reply_to_message_id{0};

    JobType type{JobType::SendText};

    std::string text;
    std::string file_id;
};

// Convert JobType enum to string
inline std::string to_string(JobType type) {
    switch (type) {
        case JobType::SendText:      return "SendText";
        case JobType::SendPhoto:     return "SendPhoto";
        case JobType::SendVoice:     return "SendVoice";
        case JobType::SendVideoNote: return "SendVideoNote";
        case JobType::SendSticker:   return "SendSticker";
        case JobType::Delete:        return "Delete";
    }
    return "Unknown";
}

// Convert string to JobType enum (supports snake_case and PascalCase)
inline std::optional<JobType> string_to_job_type(std::string_view str) {
    if (str == "SendText" || str == "send_text" || str == "text") {
        return JobType::SendText;
    } else if (str == "SendPhoto" || str == "send_photo" || str == "photo") {
        return JobType::SendPhoto;
    } else if (str == "SendVoice" || str == "send_voice" || str == "voice") {
        return JobType::SendVoice;
    } else if (str == "SendVideoNote" || str == "send_video_note" || str == "video_note") {
        return JobType::SendVideoNote;
    } else if (str == "SendSticker" || str == "send_sticker" || str == "sticker") {
        return JobType::SendSticker;
    } else if (str == "Delete" || str == "delete") {
        return JobType::Delete;
    }
    return std::nullopt;
}

// JSON deserialization for Job
inline void from_json(const nlohmann::json& j, Job& job) {
    j.at("chat_id").get_to(job.chat_id);
    j.at("message_id").get_to(job.message_id);

    std::string type_str = j.at("type").get<std::string>();
    auto parsed_type = string_to_job_type(type_str);
    if (!parsed_type.has_value()) {
        throw std::invalid_argument("Unknown job type: " + type_str);
    }
    job.type = parsed_type.value();

    // Optional fields with defaults
    job.reply_to_message_id = j.value("reply_to_message_id", int64_t{0});
    job.text = j.value("text", std::string{});
    job.file_id = j.value("file_id", std::string{});
}

// JSON serialization for Job
inline void to_json(nlohmann::json& j, const Job& job) {
    j = nlohmann::json{
        {"chat_id", job.chat_id},
        {"message_id", job.message_id},
        {"reply_to_message_id", job.reply_to_message_id},
        {"type", to_string(job.type)},
        {"text", job.text},
        {"file_id", job.file_id}
    };
}
