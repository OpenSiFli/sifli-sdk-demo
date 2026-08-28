#!/bin/sh
set -eu

sdk_dir=${SIFLI_SDK:-/Users/wangmian/SiFli-SDK/main}
source_file="$sdk_dir/external/mbedtls_228/ports/src/tls_client.c"
connect_body=$(sed -n '/^int mbedtls_client_connect(/,/^}/p' "$source_file")
verify_line=$(printf '%s\n' "$connect_body" | nl -ba |
    awk '/mbedtls_ssl_certificate_verify/ { print $1; exit }')
want_line=$(printf '%s\n' "$connect_body" | nl -ba |
    awk '/MBEDTLS_ERR_SSL_WANT_READ/ { print $1; exit }')

test -n "$verify_line"
test -n "$want_line"
test "$verify_line" -gt "$want_line"

ssl_source="$sdk_dir/external/mbedtls_228/mbedtls/library/ssl_tls.c"
verify_accessor=$(sed -n '/^uint32_t mbedtls_ssl_get_verify_result(/,/^}/p' "$ssl_source")
negotiate_line=$(printf '%s\n' "$verify_accessor" | nl -ba |
    awk '/session_negotiate/ { print $1; exit }')
session_line=$(printf '%s\n' "$verify_accessor" | nl -ba |
    awk '/session->verify_result/ { print $1; exit }')

test -n "$negotiate_line"
test -n "$session_line"
test "$negotiate_line" -lt "$session_line"
