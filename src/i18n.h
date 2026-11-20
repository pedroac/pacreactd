#ifndef I18N_H
#define I18N_H

/* pacreactd has no screen and says little, in English; this is only what the shared code includes. */
#define _(s) (s)
#define N_(s) (s)
#define ngettext(one, many, n) ((n) == 1 ? (one) : (many))

#endif
