// A consumer an earlier --consumers pass already converted: the library
// include is gone and only the module import remains, so this file cannot be
// parsed until the import is reconstructed (see
// demodularize_consumer_source).
import reconv_lib.producer;

void use_internal() {
  reconv_lib::internal::Helper h;
  (void)h;
}
