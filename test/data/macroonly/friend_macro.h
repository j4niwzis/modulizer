#ifndef MACROONLY_FRIEND_MACRO_H
#define MACROONLY_FRIEND_MACRO_H

// Nothing but a macro. There is no declaration here to export, so there is
// nothing for a module to carry — and a consumer that uses the macro still
// needs it.
#define MACROONLY_BEFRIEND(suite, name) friend class suite##_##name##_Fixture

#endif // MACROONLY_FRIEND_MACRO_H
