#!/bin/sh
set -eu

project=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
api=${1:?usage: run.sh /path/to/jakarta.jms-api-3.1.0.jar}
if [ -n "${JAVA_HOME:-}" ]; then
    java="$JAVA_HOME/bin/java"
    javac="$JAVA_HOME/bin/javac"
else
    java=java
    javac=javac
fi
classes=$(mktemp -d)
trap 'rm -rf "$classes"' EXIT
"$java" -version
"$javac" --release 11 -classpath "$api" -d "$classes" \
    "$project"/clients/java/src/main/java/tinymq/jms/*.java "$project"/tests/java/MessageContractTest.java
"$java" -ea -cp "$classes:$api" MessageContractTest "$project/tests/MessageContract.tsv"
