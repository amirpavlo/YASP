#!/bin/bash

root_dir=$PWD
install_dir=$PWD/sphinxinstall

#build YASP
export PKG_CONFIG_PATH=$install_dir/lib/pkgconfig/
gcc -Wall -Werror -g -o src/yasp src/yasp.c src/cJSON.c -I $root_dir/pocketsphinx/src/libpocketsphinx/  \
    -I $root_dir/include -I $root_dir/sphinxbase/include/sphinxbase/ \
    -DMODELDIR=\"`pkg-config --variable=modeldir pocketsphinx`\" \
    `pkg-config --cflags --libs pocketsphinx sphinxbase`

gcc -Wall -Werror -g -c -fPIC src/yasp.c src/cJSON.c src/yasp_wrap.c \
    -I /usr/include/python3.7/ \
    -I $root_dir/pocketsphinx/src/libpocketsphinx/  \
    -I $root_dir/include -I $root_dir/sphinxbase/include/sphinxbase/ \
    -DMODELDIR=\"`pkg-config --variable=modeldir pocketsphinx`\" \
    `pkg-config --cflags pocketsphinx sphinxbase`

ld -shared yasp.o cJSON.o yasp_wrap.o -o _yasp.so \
   `pkg-config --libs pocketsphinx sphinxbase`

mv *.o src/
mv *.so src/
