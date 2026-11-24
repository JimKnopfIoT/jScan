#pragma once

#include "scanworker.h"

#include <QUrl>
#include <atomic>
#include <functional>

// Native client for eSCL / AirScan network scanners (HP, Canon, Epson, Brother …).
// Plain HTTP(S) via QtNetwork, so it works the same on Linux and Windows.
//
// It is deliberately patient: many printers answer 503 ("not ready") while they
// warm up or feed paper, and some drop off Wi-Fi for a few seconds. SANE's own
// escl backend treats the first 503 as "no documents"; this client retries.
//
// All calls are blocking and must run in the scan worker thread.
namespace Escl {

// SANE-style device name for a base URL, e.g. "escl:https://192.168.10.29:443".
bool handles(const QString &deviceName);
QUrl baseUrl(const QString &deviceName);

QString queryCaps(const QString &deviceName, DeviceCaps *caps);

struct Callbacks {
    std::function<void(const QString &)> status;
    std::function<void(const QImage &)> page;
};

// Returns the number of pages delivered; *error is empty on success.
int scan(const ScanSettings &s, const DeviceCaps &caps, const std::atomic_bool &cancel,
         const Callbacks &cb, QString *error);

} // namespace Escl
