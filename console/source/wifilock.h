#pragma once
#include <stdbool.h>

/*
 * Keeps the Wi-Fi alive while the lid is closed.
 *
 * When the shell closes, the network daemon manager (NDM) hands the radio to
 * the StreetPass/SpotPass daemons, which drops the infrastructure connection.
 * Like online games do, we take the "infrastructure" exclusive state, lock it
 * and suspend the background daemons for as long as the tracker runs.
 */
void wifilock_begin(void);
void wifilock_end(void);
/* true if NDM accepted the exclusive + locked state */
bool wifilock_active(void);
/* Asks the system to (re)connect to a saved access point, asynchronously. */
void wifilock_reconnect(void);
