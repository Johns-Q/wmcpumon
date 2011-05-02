#
#	@file Makefile		@brief CPU system monitor dockapp.
#
#	Copyright (c) 2010, 2011 by Lutz Sammer.  All Rights Reserved.
#
#	Contributor(s):
#
#	License: AGPLv3
#
#	This program is free software: you can redistribute it and/or modify
#	it under the terms of the GNU Affero General Public License as
#	published by the Free Software Foundation, either version 3 of the
#	License.
#
#	This program is distributed in the hope that it will be useful,
#	but WITHOUT ANY WARRANTY; without even the implied warranty of
#	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#	GNU Affero General Public License for more details.
#
#	$Id$
#----------------------------------------------------------------------------

VERSION =	"1.02"
GIT_REV =	$(shell git describe --always 2>/dev/null)

CC=	gcc
OPTIM=	-march=native -O2 -fomit-frame-pointer
CFLAGS= $(OPTIM) -W -Wall -Wextra -g -pipe \
	-DVERSION='$(VERSION)' $(if $(GIT_REV), -DGIT_REV='"$(GIT_REV)"')
#STATIC= --static
LIBS=	$(STATIC) `pkg-config --libs $(STATIC) xcb-util xcb-atom xcb-event \
	xcb-icccm xcb-screensaver xcb-shape xcb-shm xcb-image xcb` -lpthread

OBJS=	wmcpumon.o
FILES=	Makefile README Changelog AGPL-3.0.txt wmcpumon.doxyfile wmcpumon.xpm \
	wmcpumon.1

all:	wmcpumon

wmcpumon:	$(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

wmcpumon.o:	wmcpumon.xpm Makefile

#----------------------------------------------------------------------------
#	Developer tools

doc:	$(SRCS) $(HDRS) wmcpumon.doxyfile
	(cat wmcpumon.doxyfile; \
	echo 'PROJECT_NUMBER=${VERSION} $(if $(GIT_REV), (GIT-$(GIT_REV)))') \
	| doxygen -

indent:
	for i in $(OBJS:.o=.c) $(HDRS); do \
		indent $$i; unexpand -a $$i > $$i.up; mv $$i.up $$i; \
	done

clean:
	-rm *.o *~

clobber:	clean
	-rm -rf wmcpumon www/html

dist:
	tar cjf wmcpumon-`date +%F-%H`.tar.bz2 --transform 's,^,wmcpumon/,' \
		$(FILES) $(OBJS:.o=.c)

install: wmcpumon wmcpumon.1
	strip --strip-unneeded -R .comment wmcpumon
	install -s wmcpumon /usr/local/bin/
	install -D wmcpumon.1 /usr/local/share/man/man1/wmcpumon.1

help:
	@echo "make all|doc|indent|clean|clobber|dist|install|help"
