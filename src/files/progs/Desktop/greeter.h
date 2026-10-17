#ifndef GREETER_H
#define GREETER_H

// Экран входа (как SDDM): блокирует до успешного входа, выставляет пользователя сеанса
void greeter_run(void);

// Первоначальная настройка (если её требует ядро): мастер создания первого пользователя
void oobe_run(void);

// Выход из сеанса: закрыть программы пользователя и снова показать экран входа
void session_logout(void);

extern int  g_session_uid;
extern int  g_session_gid;
extern char g_session_name[32];

#endif
