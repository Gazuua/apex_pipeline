// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

#include <apex/shared/adapters/pg/pg_transaction.hpp>

namespace apex::shared::adapters::pg
{

// Explicit instantiation for PgConnection — ensures linker symbols exist
// even though all methods are defined in the header template.
template class PgTransactionT<PgConnection>;

} // namespace apex::shared::adapters::pg
