#!/usr/bin/env bash
set -ex
git -C ../../external/prjxray-db fetch https://github.com/daveshah1/prjxray-db-7020 7020
git -C ../../external/prjxray-db checkout FETCH_HEAD

