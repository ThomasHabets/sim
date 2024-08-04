fn main() {
    // TODO: create symlink from src/simproto.proto -> ../src/simproto.proto
    protobuf_codegen::Codegen::new()
        .cargo_out_dir("protos")
        .include("src")
        .input("src/simproto.proto")
        .run_from_script();
}
