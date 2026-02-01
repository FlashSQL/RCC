#ifndef RCC_H
#define RCC_H

#include "access/xact.h"
#include "storage/lwlock.h"

/* Shared Memory Size & Initialization */
extern void InitTxnDependency(void);
extern Size RccShmemSize(void);
extern void RccShmemInit(void);

/* Dependency Graph Operations */
extern void RecordReadDependency(TransactionId xmin, TransactionId myxid);
extern void CleanupTxnDependency(void);
extern bool CheckAbortedDependencies(void);
extern bool WaitForTxnDependencies(void);

#endif /* RCC_H */
