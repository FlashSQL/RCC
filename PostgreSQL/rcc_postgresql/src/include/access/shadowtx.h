#ifndef SHADOWTX_H
#define SHADOWTX_H

#include "access/xact.h"
#include "storage/lwlock.h"

/* Shared Memory Size & Initialization */
extern void InitTxnDependency(void);
extern Size ShadowTxShmemSize(void);
extern void ShadowTxShmemInit(void);

/* Dependency Graph Operations */
extern void RecordReadDependency(TransactionId xmin, TransactionId myxid);
extern void CleanupTxnDependency(void);
extern bool CheckAbortedDependencies(void);
extern bool WaitForTxnDependencies(void);

#endif /* SHADOWTX_H */
