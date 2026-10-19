/**
 * @file crt_stubs.c
 * @brief Empty _init/_fini. We link with -nostartfiles, so the C runtime's crti/crtn that
 *        normally define these are absent, but newlib's __libc_init_array still calls _init.
 */

void _init(void);
void _fini(void);

void _init(void)
{
}

void _fini(void)
{
}
