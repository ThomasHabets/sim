fn main() {
    protobuf_codegen::Codegen::new()
        .cargo_out_dir("protos")
        .include("src")
        .input("src/simproto.proto")
        .run_from_script();
    cc::Build::new()
        .file("src/peer_cred.c")
        .compile("get_peer_uid");
}
