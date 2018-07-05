#include <afina/coroutine/Engine.h>

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

namespace Afina {
namespace Coroutine {

void Engine::Store(context &ctx) {
    char stack_top;
    ctx.Low = StackBottom;
    ctx.High = StackBottom;

    if (ctx.Low > &stack_top) {
        ctx.Low = &stack_top;
    } else {
        ctx.High = &stack_top;
    }

    uint64_t stack_size = ctx.High - ctx.Low;

    if (stack_size > ctx.Stack.second) {
        delete[] ctx.Stack.first;
        ctx.Stack.first = new char[stack_size];
        ctx.Stack.second = stack_size;
    }
    memcpy(ctx.Stack.first, ctx.Low, stack_size);
}

void Engine::Restore(context &ctx) {
    char stack_top;
    if ((&stack_top >= ctx.Low) && (&stack_top <= ctx.High)){
        Restore(ctx);
    }

    memcpy(ctx.Low, ctx.Stack.first, ctx.Stack.second);
    longjmp(ctx.Environment, 1);
}


void Engine::yield() {
    context* routine = alive;

    if (routine == cur_routine && routine != nullptr) {
        routine = routine -> next;
    }

    if (routine){
        sched(routine);
    } else {
        return;
    }
}

void Engine::sched(void *routine_) {
    context* ctx = reinterpret_cast<context*>(routine_);
    if (cur_routine != nullptr){
        if (setjmp(cur_routine->Environment) != 0) {
            return;
        }
        Store(*cur_routine);
    }
    cur_routine = ctx;
    Restore(*cur_routine);
}


} // namespace Coroutine
} // namespace Afina
