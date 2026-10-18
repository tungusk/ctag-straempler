#pragma once

void startRestAPI();
void stopRestAPI();
// teleremote kill switch (System→Settings→Remote); applied live, persisted
// as settings.remote in CONFIG.JSN (read at server start, default on)
void rest_remote_enable(int on);
// web lock (System→Settings→Web Lock, web Settings card): settings.weblock +
// settings.webkey; key NULL = keep the stored one. Inactive while no key is set.
void rest_weblock_set(int on, const char *key);
int  rest_weblock_active(void);

void rest_files_cache_reserve(void);   // boot: the /files sidecar cache, before any machine
