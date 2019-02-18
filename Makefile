# YASP Makefile
# Collect the files which need to be SWIGified
# SWIGify the files
# Compile a library and a binary
#
INSTALL_DIR=$(PWD)/sphinxinstall
PKG_CONFIG_PATH := $(INSTALL_DIR)/lib/pkgconfig/
CC=gcc
LD=ld
SWIG_BIN=swig
CFLAGS=-g -Wall -Werror -fPIC -c
SPHINX_INCLUDE=$(shell export PKG_CONFIG_PATH=$(INSTALL_DIR)/lib/pkgconfig/;pkg-config --cflags pocketsphinx sphinxbase)
ROOT_DIR=$(PWD)
INCLUDE=-I/usr/include/python3.7 -I /usr/include/python3.7/ -I $(ROOT_DIR)/pocketsphinx/src/libpocketsphinx/ -I $(ROOT_DIR)/include -I $(ROOT_DIR)/sphinxbase/include/sphinxbase/ $(SPHINX_INCLUDE)
SPHINX_LDFLAGS=$(shell export PKG_CONFIG_PATH=$(INSTALL_DIR)/lib/pkgconfig/;pkg-config --libs pocketsphinx sphinxbase)
SPHINX_MODELDIR=$(shell export PKG_CONFIG_PATH=$(INSTALL_DIR)/lib/pkgconfig/;pkg-config --variable=modeldir pocketsphinx)
LDFLAGS=$(SPHINX_LDFLAGS)
SOURCES=src/yasp.c src/cJSON.c
SOURCES_LIB=src/yasp.c src/cJSON.c src/yasp_wrap.c
SWIG_FILES=$(wildcard src/*.i)
SWIG_PY_FILES=$(wildcard src/*.py)
SWIG_SRCS=$(wildcard src/*_wrap.c)
OBJECTS=$(SOURCES:.c=.o)
OBJECTS_LIB=$(SOURCES_LIB:.c=.o)
SWIG_OBJS=$(SWIG_SRCS:.c=.o)
EXECUTABLE=src/yasp
PYTHON_YASP_LIB=src/_yasp.so

all: swig $(EXECUTABLE) copy
check:
	@echo "swig files: $(SWIG_FILES)"

swig_gen:
	$(SWIG_BIN) -python $(SWIG_FILES)
	@ls src/

python_link:
	$(LD) -shared $(LDFLAGS) src/*.o -o $(PYTHON_YASP_LIB)

build_swig_shared: $(OBJECTS_LIB) python_link

swig: swig_gen
	@echo "calling make swig"
	$(MAKE) build_swig_shared

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDE) -DMODELDIR=\"$(SPHINX_MODELDIR)\" $< -o $@

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(OBJECTS) -DMODELDIR=\"$(SPHINX_MODELDIR)\" -o $@ $(LDFLAGS)

package:
	@mkdir -p yaspinstall
	@rm -Rf yaspinstall/*
	@cp -Rf sphinxinstall/ yaspinstall/
	@cp -Rf yaspbin/ yaspinstall/
	@cp -Rf run_python yaspinstall/
	@cp -Rf run yaspinstall/
	@tar -zcf yasp-package.tar.gz yaspinstall
	@rm -Rf yaspinstall/

copy:
	@mkdir -p yaspbin
	@rm -Rf yaspbin/*
	@/bin/cp -Rf src/yasp yaspbin/
	@/bin/cp -Rf src/yasp.py yaspbin/
	@/bin/cp -Rf src/_yasp.so yaspbin/
	@/bin/cp -Rf src/yasp_setup.py yaspbin/

clean:
	@rm -Rf yaspbin/ yaspinstall/ src/*.o src/*.so src/yasp src/yasp.py* src/*_wrap.c yasp-package.tar.gz

