TEMPLATE = subdirs
CONFIG += ordered

runtime.file = $$PWD/tryx-panorama.pro
runtime.makefile = Makefile.runtime
cli.file = $$PWD/tryx-cli.pro
cli.makefile = Makefile.cli
cli.depends = runtime
quick.file = $$PWD/tryx-panorama-quick.pro
quick.makefile = Makefile.quick
quick.depends = runtime cli

SUBDIRS += runtime cli quick

# qmake's subdirs template propagates build/install/clean targets, but not
# project-specific test targets. Keep one package-facing check entry point and
# let each child project own its test implementation.
aggregate_check.target = package-check
aggregate_check.depends = all
aggregate_check.commands = \
    $(MAKE) -f Makefile.runtime check && \
    $(MAKE) -f Makefile.cli cli-check && \
    $(MAKE) -f Makefile.quick quick-check && \
    sh $$shell_path($$PWD/tests/check_translation_catalog.sh) && \
    sh $$shell_path($$PWD/tests/check_runtime_refactor_baseline.sh)
QMAKE_EXTRA_TARGETS += aggregate_check

DISTFILES += \
    tests/check_translation_catalog.sh \
    tests/check_runtime_refactor_baseline.sh \
    tests/runtime-refactor-baseline.md
