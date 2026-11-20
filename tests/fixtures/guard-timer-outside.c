/* Fixture for test-shader-syntax: a timer outside the self-test block must fail the guard. */
#ifdef MFD_ENABLE_SELF_TEST
static void harness(void) {}
#endif

static void production(void)
{
	/* a QTimer here is production code */
}
