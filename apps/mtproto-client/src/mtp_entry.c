/*
 * mtp_entry — the symbol the native loader looks for.
 *
 * jpp_app_entry is called once on a dedicated FreeRTOS task and blocks until it
 * returns. No jpp_app_task_entry is exported: the client has nothing useful to do
 * headless, since a background run cannot raise the consent prompt that
 * network.connect needs and would just fail.
 */
#include "jpp_sdk_bridge.h"

#include "mtp_app.h"

#pragma GCC visibility push(hidden)

/*
 * The one symbol the loader resolves by name, so it is the one definition in
 * this app that keeps default ELF visibility — everything else is hidden (see
 * NOTES.md, "Pass three").
 */
__attribute__((visibility("default")))
void jpp_app_entry(jpp_sdk_context_t *ctx)
{
    mtp_app_run(ctx);
}

#pragma GCC visibility pop
