#pragma once
#include <stdint.h>
#include <stddef.h>

enum FtpReceiveKind { FTP_BASE=0, FTP_UPDATE=1, FTP_DLC=2 };

struct FtpInboxItem {
    int kind;
    uint64_t bytes;
    char name[96];
    char path[256];
};

struct FtpReceiverSnapshot {
    bool running;
    uint16_t port;
    uint32_t filesReceived;
    uint64_t bytesReceived;
    int32_t lastError;
    char ip[32];
    char lastFile[96];
    char lastKind[16];
};

// Local-network receiver for user-authorized PKG/homebrew transfers.
// FTP credentials are intentionally anonymous; bind is LAN-only and files
// are confined to Peppy Store's inbox directories.
bool ftpReceiverStart();
void ftpReceiverStop();
FtpReceiverSnapshot ftpReceiverSnapshot();

// Enumerates completed PKGs in /data/peppy-store/inbox/{base,update,dlc}.
// Partial .part uploads are never exposed to the installer.
int ftpInboxList(FtpInboxItem* items, int capacity);
