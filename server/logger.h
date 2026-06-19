#ifndef LOGGER_H
#define LOGGER_H

/* Apre (o crea) il file di log. Deve essere chiamato una sola volta. */
void log_init(const char *path);

/* Scrive una riga nel log con timestamp ISO-8601. */
void log_write(const char *fmt, ...);

/* Chiude il file di log. */
void log_close(void);

#endif /* LOGGER_H */
