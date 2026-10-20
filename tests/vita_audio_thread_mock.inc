struct SDL_mutex { std::mutex value; };
struct SDL_cond { std::condition_variable value; };
struct SDL_Thread { std::thread value; int result = 0; };

namespace mock {
int live_mutex = 0, live_cond = 0, live_thread = 0;
int allocation = 0, fail_at = 0;
bool fail() { return ++allocation == fail_at; }
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void reset(int fail_at = 0) {
    require(!live_mutex && !live_cond && !live_thread, "all worker resources released");
    allocation = 0; mock::fail_at = fail_at;
}
}
SDL_mutex *SDL_CreateMutex() {
    if (mock::fail()) return nullptr;
    ++mock::live_mutex; return new SDL_mutex;
}
void SDL_DestroyMutex(SDL_mutex *p) { --mock::live_mutex; delete p; }
int SDL_LockMutex(SDL_mutex *p) { p->value.lock(); return 0; }
int SDL_UnlockMutex(SDL_mutex *p) { p->value.unlock(); return 0; }
SDL_cond *SDL_CreateCond() {
    if (mock::fail()) return nullptr;
    ++mock::live_cond; return new SDL_cond;
}
void SDL_DestroyCond(SDL_cond *p) { --mock::live_cond; delete p; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m) {
    std::unique_lock<std::mutex> lock(m->value, std::adopt_lock);
    c->value.wait(lock); lock.release(); return 0;
}
int SDL_CondSignal(SDL_cond *p) { p->value.notify_one(); return 0; }
SDL_Thread *SDL_CreateThreadWithStackSize(int (*fn)(void *), const char *, size_t stack, void *p) {
    mock::require(stack == 512u * 1024u, "native sound worker has explicit 512 KiB stack");
    if (mock::fail()) return nullptr;
    auto *thread = new SDL_Thread;
    thread->value = std::thread([=] { thread->result = fn(p); });
    ++mock::live_thread; return thread;
}
void SDL_WaitThread(SDL_Thread *thread, int *result) {
    thread->value.join();
    if (result) *result = thread->result;
    --mock::live_thread; delete thread;
}
