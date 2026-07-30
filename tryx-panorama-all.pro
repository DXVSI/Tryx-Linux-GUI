TEMPLATE = subdirs
CONFIG += ordered

runtime.file = $$PWD/tryx-panorama.pro
runtime.makefile = Makefile.runtime
quick.file = $$PWD/tryx-panorama-quick.pro
quick.makefile = Makefile.quick
quick.depends = runtime

SUBDIRS += runtime quick

# qmake's subdirs template propagates build/install/clean targets, but not
# project-specific test targets. Keep one package-facing check entry point and
# let each child project own its test implementation.
aggregate_check.target = package-check
aggregate_check.depends = all
aggregate_check.commands = \
    $(MAKE) -f Makefile.runtime check && \
    $(MAKE) -f Makefile.quick quick-check
QMAKE_EXTRA_TARGETS += aggregate_check
