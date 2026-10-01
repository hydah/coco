#pragma once

// The handle types of st.h, so public headers can hold them without including it. The
// typedefs are identical to st.h's, which may be included as well.
typedef struct _st_thread *st_thread_t;
typedef struct _st_cond *st_cond_t;
typedef struct _st_mutex *st_mutex_t;
typedef struct _st_netfd *st_netfd_t;
