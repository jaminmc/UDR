#!/bin/bash
# Prefer an exact annotated/lightweight tag (e.g. v0.9.5). Fall back to
# describe --long when building from a non-tagged commit.
if [ -d ../.git ]; then
  if ver=$(git describe --tags --exact-match 2>/dev/null); then
    echo "$ver" > version
  else
    git describe --tags --long > version
  fi
fi

version=`cat version`

printf "#ifndef VERSION_H\n#define VERSION_H\n" > version.h
printf "static const char * version = \"%s" "$version" >> version.h
printf "\";\n#endif" >> version.h
