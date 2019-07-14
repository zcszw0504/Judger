#include "common/messages.hpp"

namespace judge::message {

task_result::task_result() : score(0) {}
task_result::task_result(unsigned judge_id, uint8_t id, uint8_t type)
    : judge_id(judge_id), id(id), type(type), run_time(0), memory_used(0), score(0) {}

}  // namespace judge::message