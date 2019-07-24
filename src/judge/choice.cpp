#include "judge/choice.hpp"
#include <glog/logging.h>
#include "worker.hpp"

namespace judge {
using namespace std;

static float judge_choice_question(const choice_question &q) {
    unsigned long std = 0, ans = 0;
    for (auto &choice : q.student_answer)
        ans |= 1 << choice;
    for (auto &choice : q.standard_answer)
        std |= 1 << choice;
    if (ans == std)
        return q.full_grade;
    else if ((ans & std) == ans)
        return q.half_grade;
    else
        return 0;
}

string choice_judger::type() {
    return "choice";
}

bool choice_judger::verify(unique_ptr<submission> &submit) {
    auto sub = dynamic_cast<choice_submission *>(submit.get());
    if (!sub) return false;
    LOG(INFO) << "Judging choice submission [" << sub->category << "-" << sub->prob_id << "-" << sub->sub_id << "]";

    return true;
}

bool choice_judger::distribute(concurrent_queue<message::client_task> &task_queue, unique_ptr<submission> &submit) {
    // 我们只需要发一个评测请求就行了，以便让 client 能调用我们的 judge 函数
    // 或者我们在 verify 的时候就评测完选择题然后返回 false 也行。
    judge::message::client_task client_task = {
        .submit = submit.get(),
        .id = 0};
    task_queue.push(client_task);
    return true;
}

void choice_judger::judge(const message::client_task &task, concurrent_queue<message::client_task> &, const string &) {
    auto submit = dynamic_cast<choice_submission *>(task.submit);

    for (auto &q : submit->questions)
        q.grade = judge_choice_question(q);

    auto &judge_server = get_judge_server_by_category(submit->category);
    judge_server.summarize(*submit);
    fire_judge_finished(*submit);
}

}  // namespace judge
