CC ?= cc
CFLAGS ?= -O2 -pipe
CPPFLAGS ?=
LDFLAGS ?=
WARNINGS = -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wconversion \
	-Wstrict-prototypes -Wmissing-prototypes

.PHONY: all clean check install
all: dmemcg-openrcd dmem-run
dmemcg-openrcd: src/dmemcg-openrcd.c src/common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c17 -o $@ src/dmemcg-openrcd.c $(LDFLAGS)
dmem-run: src/dmem-run.c src/common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c17 -o $@ src/dmem-run.c $(LDFLAGS)
check: all
	sh ./tests/static-checks.sh
install: all
	install -Dm0755 dmemcg-openrcd "$(DESTDIR)/usr/sbin/dmemcg-openrcd"
	install -Dm0755 dmem-run "$(DESTDIR)/usr/bin/dmem-run"
	install -Dm0755 openrc/dmemcg-openrc "$(DESTDIR)/etc/init.d/dmemcg-openrc"
	install -Dm0644 openrc/dmemcg-openrc.conf "$(DESTDIR)/etc/conf.d/dmemcg-openrc"
	install -Dm0644 LICENSE "$(DESTDIR)/usr/share/licenses/dmemcg-openrc/LICENSE"
clean:
	rm -f dmemcg-openrcd dmem-run
