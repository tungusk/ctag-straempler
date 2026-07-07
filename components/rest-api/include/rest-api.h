#pragma once

void startRestAPI();
void stopRestAPI();
// teleremote kill switch (System→Settings→Remote); applied live, persisted
// as settings.remote in CONFIG.JSN (read at server start, default on)
void rest_remote_enable(int on);
