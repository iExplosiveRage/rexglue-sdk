/**
 * @file        net/session.h
 * @brief       Whether the game is in an online session
 */
#pragma once

namespace rex::net {

// True from when the game creates or joins an Xbox LIVE / system link session
// (XSessionCreate) until it deletes it - so while it plays online. Thread-safe.
bool IsGameSessionOpen();

// For the XAM session implementation.
void SetGameSessionOpen(bool open);

}  // namespace rex::net
