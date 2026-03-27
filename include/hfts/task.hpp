#ifndef hfts_task_h
#define hfts_task_h

#include <functional>

namespace hfts {

class Task {
public:
    using Function = std::function<void()>;

    enum class Flag {
        None = 0,

        // The task must be executed on the thread 
        // that is currently submitting it.
        SameThread = 1,
    };

    inline Task();
    inline Task(const Task&);
    inline Task(Task&&);
    inline Task(const Function& function, Flag flag = Flag::None);
    inline Task(Function&& function, Flag flag = Flag::None);
    inline Task& operator=(const Task&);
    inline Task& operator=(Task&&);
    inline Task& operator=(const Function&);
    inline Task& operator=(Function&&);

    inline operator bool() const;
    inline void operator ()() const;

    // Return true if Task was created with the given flag
    inline bool is(Flag f) const;

private:
    Function func;
    Flag flag = Flag::None;
};

Task::Task() = default;
Task::Task(const Task& t) : func(t.func), flag(t.flag) {}
Task::Task(Task&& t) : func(std::move(t.func)), flag(t.flag) {}
Task::Task(const Function& function, Flag flag_)
    : func(function), flag(flag_) {}
Task::Task(Function&& function, Flag flag_)
    : func(std::move(function)), flag(flag_) {}

Task& Task::operator=(const Task& t) {
    func = t.func;
    flag = t.flag;
    return *this;
}

Task& Task::operator=(Task&& t) {
    func = std::move(t.func);
    flag = t.flag;
    return *this;
}

Task& Task::operator=(const Function& function) {
    func = function;
    flag = Flag::None;
    return *this;
}

Task& Task::operator=(Function&& function) {
    func = std::move(function);
    flag = Flag::None;
    return *this;
}

Task::operator bool() const {
    return func.operator bool();
}

void Task::operator ()() const {
    func();
}

bool Task::is(Flag f) const {
    return (static_cast<int>(flag) & static_cast<int>(f)) == static_cast<int>(f);
}

}
#endif