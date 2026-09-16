/* Convert the read-aloud voice model from ONNX to ONNX Runtime's .ort format, the
 * only format the minimal runtime linked into openchimed reads (ARCH-111).
 *
 *   tts_convert MODEL.onnx OUT.ort amd64|arm
 *
 * Build-time only, run by scripts/build_kitten.sh against the prebuilt full ONNX
 * Runtime (scripts/build_onnxruntime.sh converter). It does what ONNX Runtime's
 * Python convert_onnx_models_to_ort.py does with --optimization_style Fixed: load
 * the model with every graph optimization on and save the optimized graph as .ort,
 * int8 QDQ allowed only for arm. Unlike there, the NCHWc layout transform is off on
 * every architecture: it bakes in the converting machine's vector width, and a model
 * converted on an AVX-512 build runner must run on an AVX2 server.
 *
 * The output is not byte-for-byte reproducible (the runtime's serializer orders
 * part of the graph header differently between runs); the weights are the same. */
#include <onnxruntime_c_api.h>

#include <stdio.h>
#include <string.h>

static const OrtApi *ort;

static int ok(OrtStatus *st, const char *what) {
    if (!st) return 1;
    fprintf(stderr, "tts_convert: %s: %s\n", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[3], "amd64") && strcmp(argv[3], "arm"))) {
        fprintf(stderr, "usage: tts_convert MODEL.onnx OUT.ort amd64|arm\n");
        return 2;
    }
    int arm = !strcmp(argv[3], "arm");
    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) { fprintf(stderr, "tts_convert: ONNX Runtime API %d unavailable\n", ORT_API_VERSION); return 1; }

    OrtEnv *env = NULL;
    OrtSessionOptions *so = NULL;
    OrtSession *session = NULL;
    int good = ok(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "tts_convert", &env), "CreateEnv") &&
               ok(ort->CreateSessionOptions(&so), "CreateSessionOptions") &&
               ok(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "optimization level") &&
               ok(ort->SetOptimizedModelFilePath(so, argv[2]), "output path") &&
               ok(ort->AddSessionConfigEntry(so, "session.save_model_format", "ORT"), "save format") &&
               ok(ort->AddSessionConfigEntry(so, "session.qdqisint8allowed", arm ? "1" : "0"), "qdq") &&
               ok(ort->AddSessionConfigEntry(so, "optimization.disable_specified_optimizers", "NchwcTransformer"), "disable NCHWc") &&
               ok(ort->CreateSession(env, argv[1], so, &session), argv[1]);
    if (session) ort->ReleaseSession(session);
    if (so) ort->ReleaseSessionOptions(so);
    if (env) ort->ReleaseEnv(env);
    if (!good) return 1;
    printf("tts_convert: %s -> %s (%s)\n", argv[1], argv[2], argv[3]);
    return 0;
}
