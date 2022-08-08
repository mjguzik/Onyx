#!/bin/sh
shopt -s nullglob
ARCH=$1
SRC_DIRS="src/* src/malloc/mallocng crt ldso"

for src in $SRC_DIRS; do
    BASE_GLOBS="$BASE_GLOBS $src/*.c"
    if [ -d "$src/$ARCH" ]; then
        ARCH_GLOBS="$ARCH_GLOBS $src/$ARCH/*.c $src/$ARCH/*.s $src/$ARCH/*.S"
    fi
done

echo "base_sources = ["
for file in $BASE_GLOBS; do
    echo "\"$file\","
done
echo "]"

echo ""

echo "arch_${ARCH}_sources = ["

for file in $ARCH_GLOBS; do
    echo "\"$file\","
done

echo "]"