use std::sync::Arc;
use room237_native_core::{host::{request_lines,write_message},protocol::{Context,dispatch}};
#[tokio::main(flavor="multi_thread",worker_threads=4)]
async fn main() {
    let args:Vec<String>=std::env::args().skip(1).collect();
    if args.first().map(String::as_str)==Some("--version") { println!("{}",room237_native_core::update::CURRENT_VERSION); return; }
    if args.first().map(String::as_str)==Some("--apply-update") {
        std::process::exit(room237_native_core::update::run_helper(args.get(1).map(String::as_str).unwrap_or("")));
    }
    let profile=match (args.first().map(String::as_str),args.get(1)) {
        (Some("--profile"),Some(path))=>std::path::PathBuf::from(path),
        _=>{ eprintln!("Usage: room237-core --profile ABSOLUTE_DIRECTORY [--legacy-profile DIRECTORY]"); std::process::exit(2); }
    };
    if !profile.is_absolute(){eprintln!("Profile must be absolute");std::process::exit(2);}
    if args.get(2).map(String::as_str)==Some("--legacy-profile") {
        if let Some(legacy)=args.get(3) { let outcome=room237_native_core::migrate::migrate_profile(&profile,std::path::Path::new(legacy)); eprintln!("profile migration: {outcome:?}"); }
    }
    let context=Arc::new(Context::new(profile).unwrap_or_else(|error|{eprintln!("{error}");std::process::exit(1)}));
    // Requests run concurrently; limit admission so a scrolling client cannot build an unbounded work queue.
    let admission=Arc::new(tokio::sync::Semaphore::new(8));
    let mut jobs=tokio::task::JoinSet::new();
    for line in request_lines() {
        let Ok(line)=line else {break};
        let request:serde_json::Value=match serde_json::from_str(&line){Ok(v)=>v,Err(e)=>{let _=write_message(&serde_json::json!({"id":null,"error":e.to_string()}));continue}};
        let context=context.clone();let permit=admission.clone().acquire_owned().await.unwrap();
        while jobs.try_join_next().is_some() {}
        jobs.spawn(async move {
            let _permit=permit;
            let id=request.get("id").cloned().unwrap_or_default();
            let response=match dispatch(&context,request["method"].as_str().unwrap_or(""),request.get("params").cloned().unwrap_or(serde_json::json!({}))).await {
                Ok(result)=>serde_json::json!({"id":id,"result":result}),
                Err(error)=>serde_json::json!({"id":id,"error":error}),
            };
            let _=write_message(&response);
        });
    }
    while jobs.join_next().await.is_some() {}
}
