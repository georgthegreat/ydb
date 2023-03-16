# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(
    Apache-2.0 AND
    Brian-Gladman-3-Clause AND
    CC0-1.0 AND
    MIT AND
    Public-Domain
)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.3.38)

ORIGINAL_SOURCE(https://github.com/aws/s2n-tls/archive/v1.3.38.tar.gz)

PEERDIR(
    contrib/libs/openssl
)

ADDINCL(
    GLOBAL contrib/restricted/aws/s2n
    GLOBAL contrib/restricted/aws/s2n/api
)

NO_COMPILER_WARNINGS()

NO_RUNTIME()

CFLAGS(
    -DS2N_CLONE_SUPPORTED
    -DS2N_FALL_THROUGH_SUPPORTED
    -DS2N_LIBCRYPTO_SUPPORTS_EVP_MD5_SHA1_HASH
    -DS2N_LIBCRYPTO_SUPPORTS_EVP_MD_CTX_SET_PKEY_CTX
    -DS2N_LIBCRYPTO_SUPPORTS_EVP_RC4
    -DS2N_MADVISE_SUPPORTED
    -DS2N___RESTRICT__SUPPORTED
)

IF (OS_LINUX)
    CFLAGS(
        -DS2N_FEATURES_AVAILABLE
    )
ENDIF()

IF (NOT MUSL)
    CFLAGS(
        -DS2N_STACKTRACE
    )
ENDIF()

IF (ARCH_X86_64)
    CFLAGS(
        -DS2N_CPUID_AVAILABLE
        -DS2N_KYBER512R3_AVX2_BMI2
    )
    SRCS(
        pq-crypto/kyber_r3/kyber512r3_basemul_avx2.S
        pq-crypto/kyber_r3/kyber512r3_fq_avx2.S
        pq-crypto/kyber_r3/kyber512r3_invntt_avx2.S
        pq-crypto/kyber_r3/kyber512r3_ntt_avx2.S
        pq-crypto/kyber_r3/kyber512r3_shuffle_avx2.S
    )
    SRC_C_AVX2(pq-crypto/kyber_r3/KeccakP-1600-times4-SIMD256_avx2.c)
    SRC_C_AVX2(pq-crypto/kyber_r3/kyber512r3_cbd_avx2.c)
    SRC_C_AVX2(pq-crypto/kyber_r3/kyber512r3_consts_avx2.c)
    SRC_C_AVX2(pq-crypto/kyber_r3/kyber512r3_fips202x4_avx2.c)
    SRC_C_AVX2(pq-crypto/kyber_r3/kyber512r3_indcpa_avx2.c)
    SRC_C_AVX2(pq-crypto/kyber_r3/kyber512r3_poly_avx2.c)
    SRC_C_AVX2(pq-crypto/kyber_r3/kyber512r3_polyvec_avx2.c)
    SRC_C_AVX2(pq-crypto/kyber_r3/kyber512r3_rejsample_avx2.c)
ENDIF()

SRCS(
    crypto/s2n_aead_cipher_aes_gcm.c
    crypto/s2n_aead_cipher_chacha20_poly1305.c
    crypto/s2n_cbc_cipher_3des.c
    crypto/s2n_cbc_cipher_aes.c
    crypto/s2n_certificate.c
    crypto/s2n_cipher.c
    crypto/s2n_composite_cipher_aes_sha.c
    crypto/s2n_crypto.c
    crypto/s2n_dhe.c
    crypto/s2n_drbg.c
    crypto/s2n_ecc_evp.c
    crypto/s2n_ecdsa.c
    crypto/s2n_evp.c
    crypto/s2n_evp_signing.c
    crypto/s2n_fips.c
    crypto/s2n_hash.c
    crypto/s2n_hkdf.c
    crypto/s2n_hmac.c
    crypto/s2n_libcrypto.c
    crypto/s2n_locking.c
    crypto/s2n_openssl_x509.c
    crypto/s2n_pkey.c
    crypto/s2n_rsa.c
    crypto/s2n_rsa_pss.c
    crypto/s2n_rsa_signing.c
    crypto/s2n_sequence.c
    crypto/s2n_stream_cipher_null.c
    crypto/s2n_stream_cipher_rc4.c
    crypto/s2n_tls13_keys.c
    error/s2n_errno.c
    pq-crypto/kyber_r3/kyber512r3_cbd.c
    pq-crypto/kyber_r3/kyber512r3_fips202.c
    pq-crypto/kyber_r3/kyber512r3_indcpa.c
    pq-crypto/kyber_r3/kyber512r3_kem.c
    pq-crypto/kyber_r3/kyber512r3_ntt.c
    pq-crypto/kyber_r3/kyber512r3_poly.c
    pq-crypto/kyber_r3/kyber512r3_polyvec.c
    pq-crypto/kyber_r3/kyber512r3_reduce.c
    pq-crypto/kyber_r3/kyber512r3_symmetric-shake.c
    pq-crypto/s2n_kyber_512_evp.c
    pq-crypto/s2n_pq.c
    pq-crypto/s2n_pq_random.c
    stuffer/s2n_stuffer.c
    stuffer/s2n_stuffer_base64.c
    stuffer/s2n_stuffer_file.c
    stuffer/s2n_stuffer_network_order.c
    stuffer/s2n_stuffer_pem.c
    stuffer/s2n_stuffer_text.c
    tls/extensions/s2n_cert_status.c
    tls/extensions/s2n_cert_status_response.c
    tls/extensions/s2n_client_alpn.c
    tls/extensions/s2n_client_cert_status_request.c
    tls/extensions/s2n_client_cookie.c
    tls/extensions/s2n_client_early_data_indication.c
    tls/extensions/s2n_client_ems.c
    tls/extensions/s2n_client_key_share.c
    tls/extensions/s2n_client_max_frag_len.c
    tls/extensions/s2n_client_pq_kem.c
    tls/extensions/s2n_client_psk.c
    tls/extensions/s2n_client_renegotiation_info.c
    tls/extensions/s2n_client_sct_list.c
    tls/extensions/s2n_client_server_name.c
    tls/extensions/s2n_client_session_ticket.c
    tls/extensions/s2n_client_signature_algorithms.c
    tls/extensions/s2n_client_supported_groups.c
    tls/extensions/s2n_client_supported_versions.c
    tls/extensions/s2n_ec_point_format.c
    tls/extensions/s2n_extension_list.c
    tls/extensions/s2n_extension_type.c
    tls/extensions/s2n_extension_type_lists.c
    tls/extensions/s2n_key_share.c
    tls/extensions/s2n_npn.c
    tls/extensions/s2n_nst_early_data_indication.c
    tls/extensions/s2n_psk_key_exchange_modes.c
    tls/extensions/s2n_quic_transport_params.c
    tls/extensions/s2n_server_alpn.c
    tls/extensions/s2n_server_cert_status_request.c
    tls/extensions/s2n_server_cookie.c
    tls/extensions/s2n_server_early_data_indication.c
    tls/extensions/s2n_server_ems.c
    tls/extensions/s2n_server_key_share.c
    tls/extensions/s2n_server_max_fragment_length.c
    tls/extensions/s2n_server_psk.c
    tls/extensions/s2n_server_renegotiation_info.c
    tls/extensions/s2n_server_sct_list.c
    tls/extensions/s2n_server_server_name.c
    tls/extensions/s2n_server_session_ticket.c
    tls/extensions/s2n_server_signature_algorithms.c
    tls/extensions/s2n_server_supported_versions.c
    tls/extensions/s2n_supported_versions.c
    tls/s2n_aead.c
    tls/s2n_alerts.c
    tls/s2n_async_pkey.c
    tls/s2n_auth_selection.c
    tls/s2n_cbc.c
    tls/s2n_change_cipher_spec.c
    tls/s2n_cipher_preferences.c
    tls/s2n_cipher_suites.c
    tls/s2n_client_cert.c
    tls/s2n_client_cert_verify.c
    tls/s2n_client_finished.c
    tls/s2n_client_hello.c
    tls/s2n_client_hello_request.c
    tls/s2n_client_key_exchange.c
    tls/s2n_config.c
    tls/s2n_connection.c
    tls/s2n_crl.c
    tls/s2n_crypto.c
    tls/s2n_early_data.c
    tls/s2n_early_data_io.c
    tls/s2n_ecc_preferences.c
    tls/s2n_encrypted_extensions.c
    tls/s2n_establish_session.c
    tls/s2n_fingerprint.c
    tls/s2n_handshake.c
    tls/s2n_handshake_hashes.c
    tls/s2n_handshake_io.c
    tls/s2n_handshake_transcript.c
    tls/s2n_handshake_type.c
    tls/s2n_kem.c
    tls/s2n_kem_preferences.c
    tls/s2n_kex.c
    tls/s2n_key_log.c
    tls/s2n_key_update.c
    tls/s2n_next_protocol.c
    tls/s2n_ocsp_stapling.c
    tls/s2n_post_handshake.c
    tls/s2n_prf.c
    tls/s2n_protocol_preferences.c
    tls/s2n_psk.c
    tls/s2n_quic_support.c
    tls/s2n_record_read.c
    tls/s2n_record_read_aead.c
    tls/s2n_record_read_cbc.c
    tls/s2n_record_read_composite.c
    tls/s2n_record_read_stream.c
    tls/s2n_record_write.c
    tls/s2n_recv.c
    tls/s2n_renegotiate.c
    tls/s2n_resume.c
    tls/s2n_security_policies.c
    tls/s2n_send.c
    tls/s2n_server_cert.c
    tls/s2n_server_cert_request.c
    tls/s2n_server_done.c
    tls/s2n_server_extensions.c
    tls/s2n_server_finished.c
    tls/s2n_server_hello.c
    tls/s2n_server_hello_retry.c
    tls/s2n_server_key_exchange.c
    tls/s2n_server_new_session_ticket.c
    tls/s2n_shutdown.c
    tls/s2n_signature_algorithms.c
    tls/s2n_signature_scheme.c
    tls/s2n_tls.c
    tls/s2n_tls13.c
    tls/s2n_tls13_certificate_verify.c
    tls/s2n_tls13_handshake.c
    tls/s2n_tls13_key_schedule.c
    tls/s2n_tls13_secrets.c
    tls/s2n_x509_validator.c
    utils/s2n_array.c
    utils/s2n_asn1_time.c
    utils/s2n_blob.c
    utils/s2n_ensure.c
    utils/s2n_fork_detection.c
    utils/s2n_init.c
    utils/s2n_map.c
    utils/s2n_mem.c
    utils/s2n_random.c
    utils/s2n_result.c
    utils/s2n_rfc5952.c
    utils/s2n_safety.c
    utils/s2n_set.c
    utils/s2n_socket.c
    utils/s2n_timer.c
)

END()
