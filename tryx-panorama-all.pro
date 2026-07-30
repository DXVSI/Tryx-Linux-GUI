TEMPLATE = subdirs
CONFIG += ordered

widgets.file = $$PWD/tryx-panorama.pro
widgets.makefile = Makefile.widgets
quick.file = $$PWD/tryx-panorama-quick.pro
quick.makefile = Makefile.quick
quick.depends = widgets

SUBDIRS += widgets quick

# qmake's subdirs template propagates build/install/clean targets, but not
# project-specific test targets. Keep one package-facing check entry point and
# let each child project own its test implementation.
aggregate_check.target = package-check
aggregate_check.depends = all
aggregate_check.commands = \
    $(MAKE) -f Makefile.widgets check && \
    $(MAKE) -f Makefile.quick quick-check
QMAKE_EXTRA_TARGETS += aggregate_check
