#pragma once

#include "../ExecutorContext.h"
#include "../../Storage/Tuple/Tuple.h"
#include "../../Common/RID.h"
#include "../Chunk.h"

namespace Database
{

    class AbstractExecutor
    {
    public:
        AbstractExecutor(ExecutorContext *exec_ctx) : exec_ctx_(exec_ctx) {}
        virtual ~AbstractExecutor() = default;

        virtual void Init() = 0;
        // Legacy Row-based Next
        virtual bool Next(Tuple *tuple, RID *rid) = 0;

        // Vectorized Batch Next
        // Returns true if a chunk is returned with count > 0.
        // Returns false if exhaust the iterator (EOF).
        virtual bool Next(Chunk &chunk)
        {
            // Default naive implementation wrapping row-based access
            // Child classes should explicitly override this for performance!
            chunk.Reset();
            // Assuming default vectors are set up correctly externally or needs overriding
            return false;
        }

        ExecutorContext *GetExecutorContext() { return exec_ctx_; }

    protected:
        ExecutorContext *exec_ctx_;
    };

} // namespace Database