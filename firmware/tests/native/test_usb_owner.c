#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "vk_usb_owner.h"

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_mutex_t notify_mutex;
    vk_usb_owner_t owner;
    void *last_notified;
    unsigned notify_count;
    unsigned uninstall_count;
    bool notify_entered;
    bool release_notify;
} fixture_t;

static void lock_owner(void *context) { assert(pthread_mutex_lock(&((fixture_t *)context)->mutex) == 0); }
static void unlock_owner(void *context) { assert(pthread_mutex_unlock(&((fixture_t *)context)->mutex) == 0); }
static void notify_owner(void *context, void *handle)
{
    fixture_t *fixture = context;
    assert(pthread_mutex_lock(&fixture->notify_mutex) == 0);
    fixture->last_notified = handle;
    ++fixture->notify_count;
    fixture->notify_entered = true;
    assert(pthread_cond_broadcast(&fixture->condition) == 0);
    while (!fixture->release_notify) assert(pthread_cond_wait(&fixture->condition, &fixture->notify_mutex) == 0);
    assert(pthread_mutex_unlock(&fixture->notify_mutex) == 0);
}

static void initialize(fixture_t *fixture)
{
    *fixture = (fixture_t){.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER, .notify_mutex = PTHREAD_MUTEX_INITIALIZER};
    vk_usb_owner_ops_t ops = {.lock = lock_owner, .unlock = unlock_owner, .notify = notify_owner, .context = fixture};
    vk_usb_owner_init(&fixture->owner, &ops);
    assert(vk_usb_owner_is_quiescent(&fixture->owner));
}

static void *request_thread(void *context)
{
    bool *result = ((void **)context)[1];
    *result = vk_usb_owner_request_stop(((void **)context)[0]);
    return NULL;
}

static void *quiesce_thread(void *context)
{
    void **values = context;
    vk_usb_owner_quiesce(values[0], values[1]);
    return NULL;
}

static bool stop_and_uninstall(fixture_t *fixture, bool stopped_signal)
{
    (void)vk_usb_owner_request_stop(&fixture->owner);
    if (!stopped_signal || !vk_usb_owner_is_quiescent(&fixture->owner)) return false;
    ++fixture->uninstall_count;
    return true;
}

static void test_normal_notify_pins_handle_until_return(void)
{
    fixture_t fixture; initialize(&fixture);
    void *handle = (void *)(uintptr_t)0x101U;
    vk_usb_owner_attach(&fixture.owner, handle);
    bool request_result = false;
    void *request_args[] = {&fixture.owner, &request_result};
    pthread_t requester;
    assert(pthread_create(&requester, NULL, request_thread, request_args) == 0);
    assert(pthread_mutex_lock(&fixture.notify_mutex) == 0);
    while (!fixture.notify_entered) assert(pthread_cond_wait(&fixture.condition, &fixture.notify_mutex) == 0);
    assert(fixture.last_notified == handle);
    assert(pthread_mutex_unlock(&fixture.notify_mutex) == 0);

    void *quiesce_args[] = {&fixture.owner, handle};
    pthread_t quitter;
    assert(pthread_create(&quitter, NULL, quiesce_thread, quiesce_args) == 0);
    assert(pthread_mutex_lock(&fixture.notify_mutex) == 0);
    fixture.release_notify = true;
    assert(pthread_cond_broadcast(&fixture.condition) == 0);
    assert(pthread_mutex_unlock(&fixture.notify_mutex) == 0);
    assert(pthread_join(requester, NULL) == 0);
    assert(pthread_join(quitter, NULL) == 0);
    assert(request_result);
    assert(fixture.notify_count == 1U);
    assert(vk_usb_owner_is_quiescent(&fixture.owner));
}

static void test_self_exit_before_public_stop_has_zero_stale_notify(void)
{
    fixture_t fixture; initialize(&fixture);
    void *handle = (void *)(uintptr_t)0x202U;
    vk_usb_owner_attach(&fixture.owner, handle);
    vk_usb_owner_quiesce(&fixture.owner, handle);
    assert(!vk_usb_owner_request_stop(&fixture.owner));
    assert(fixture.notify_count == 0U);
    assert(stop_and_uninstall(&fixture, true));
    assert(fixture.uninstall_count == 1U);
}

static void test_timeout_never_uninstalls_live_owner(void)
{
    fixture_t fixture; initialize(&fixture);
    vk_usb_owner_attach(&fixture.owner, (void *)(uintptr_t)0x303U);
    fixture.release_notify = true;
    assert(!stop_and_uninstall(&fixture, false));
    assert(fixture.notify_count == 1U);
    assert(fixture.uninstall_count == 0U);
    assert(!vk_usb_owner_is_quiescent(&fixture.owner));
}

static void test_handle_reuse_sentinel(void)
{
    fixture_t fixture; initialize(&fixture);
    void *old_handle = (void *)(uintptr_t)0x404U;
    void *reused_handle = (void *)(uintptr_t)0x505U;
    vk_usb_owner_attach(&fixture.owner, old_handle);
    vk_usb_owner_quiesce(&fixture.owner, old_handle);
    assert(!vk_usb_owner_request_stop(&fixture.owner));
    assert(fixture.notify_count == 0U);
    vk_usb_owner_attach(&fixture.owner, reused_handle);
    /* A late old-task exit cannot clear the new owner. */
    vk_usb_owner_quiesce(&fixture.owner, old_handle);
    fixture.release_notify = true;
    assert(vk_usb_owner_request_stop(&fixture.owner));
    assert(fixture.notify_count == 1U);
    assert(fixture.last_notified == reused_handle);
    assert(!vk_usb_owner_is_quiescent(&fixture.owner));
    vk_usb_owner_quiesce(&fixture.owner, reused_handle);
    assert(vk_usb_owner_is_quiescent(&fixture.owner));
}

int main(void)
{
    test_normal_notify_pins_handle_until_return();
    test_self_exit_before_public_stop_has_zero_stale_notify();
    test_timeout_never_uninstalls_live_owner();
    test_handle_reuse_sentinel();
    puts("vk_usb owner native tests passed");
}
