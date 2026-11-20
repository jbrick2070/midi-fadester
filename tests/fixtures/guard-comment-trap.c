/* Fixture for test-shader-syntax: the harness is behind #ifdef MFD_ENABLE_SELF_TEST, but this comment opens no
 * block, so the timer in the code below is still found. */
static void production(void)
{
	/* QTimer in production code */
}
#if defined(_WIN32)
static int windows_only(void)
{
	return 0;
}
#endif
