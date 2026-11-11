#pragma once

#include <QMetaType>

namespace span::core {

// Shared terminal state for user-visible operations. Running is represented by
// progressStarted/progressChanged; this enum is emitted only when an operation
// reaches a terminal state.
enum class OperationOutcome {
    Succeeded = 0,
    Failed,
    Cancelled,
};

} // namespace span::core

Q_DECLARE_METATYPE(span::core::OperationOutcome)
