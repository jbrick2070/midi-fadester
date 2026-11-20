/* Fixture for test-shader-syntax: a timer inside the self-test block is the one place one may appear. */
static int production_value(void)
{
	return 1;
}

#ifdef MFD_ENABLE_SELF_TEST
#if defined(_WIN32)
static void wait_a_little(void)
{
	/* QTimer and Sleep( are allowed here */
}
#endif
#endif

static int more_production(void)
{
	return production_value();
}
