#pragma once
// Shim for MinGW (the Windows SDK header is missing): same layout as the SDK definition.
typedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;
