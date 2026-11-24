// Platform-independent parts of ScanWorker.
#include "scanworker.h"

bool ScanWorker::isFeeder(const QString &source)
{
    return source.contains(QLatin1String("ADF"), Qt::CaseInsensitive)
           || source.contains(QLatin1String("Feeder"), Qt::CaseInsensitive)
           || source.contains(QLatin1String("Duplex"), Qt::CaseInsensitive)
           || source.contains(QLatin1String("Card"), Qt::CaseInsensitive);
}

