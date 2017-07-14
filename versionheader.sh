#!/bin/sh

version=$(cat version 2>/dev/null)
if test -z "$version"; then version="unspecified"; fi
set -- $(sed -e 's/[^0-9a-z][^0-9a-z]*/ /g' <version 2>/dev/null)
major_version="$1"
minor_version="$2"
micro_version="$3"
localrev="$4"
if test -z "$major_version"; then major_version=0; fi
if test -z "$minor_version"; then minor_version=0; fi
if test -z "$micro_version"; then micro_version=0; fi
if test -z "$localrev"; then localrev=0; fi
globalrev=$(( $localrev | ($micro_version << 16) | ($minor_version << 20) | ($major_version << 28) ))

cat << EOF
#define PROJECT_VERSION                 "$version"
#define PROJECT_VERSION_MAJOR           $major_version
#define PROJECT_VERSION_MINOR           $minor_version
#define PROJECT_VERSION_MICRO           $micro_version
#define PROJECT_VERSION_REVISION        ${globalrev}UL
#define PROJECT_VERSION_COMMIFIED       "$major_version,$minor_version,$micro_version"
EOF
