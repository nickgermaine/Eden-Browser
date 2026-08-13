use std::os::raw::c_char;

static VERSION: &[u8] = b"eden-servo 0.1.0\0";

#[no_mangle]
pub extern "C" fn eden_servo_runtime_available() -> bool {
    cfg!(feature = "servo")
}

#[no_mangle]
pub extern "C" fn eden_servo_version() -> *const c_char {
    VERSION.as_ptr().cast()
}
