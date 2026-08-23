#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

#include "socket_utils.h"
#include "window.h"

Display *iniciarDisplay(void)
{
    Display *display = XOpenDisplay(NULL);

    if (display == NULL)
    {
        fprintf(stderr, "No se pudo abrir el servidor X11.\n");
        exit(EXIT_FAILURE);
    }

    return display;
}

Window crearVentana(Display *display, Atom *wmDelete)
{
    int screen = DefaultScreen(display);

    Window window = XCreateSimpleWindow(
        display,
        RootWindow(display, screen),
        10,
        10,
        400,
        200,
        1,
        BlackPixel(display, screen),
        WhitePixel(display, screen));

    XSelectInput(display, window, ExposureMask | KeyPressMask);

    /* Le pedimos al gestor de ventanas que nos avise con un evento
       (ClientMessage) en vez de simplemente matar la conexion cuando
       el usuario presiona el boton "X". Sin esto, cerrar con el mouse
       puede dejar la ventana colgada esperando eventos para siempre,
       y el launcher se quedaria esperandola tambien. */
    *wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, wmDelete, 1);

    XMapWindow(display, window);

    return window;
}

void procesarEventos(Display *display, Atom wmDelete, int socketServidor)
{
    XEvent event;
    int salir = 0;

    /* Se usa una variable "salir" en vez de while(1) con un break
       escondido en medio: asi la condicion de salida queda visible
       en el encabezado del ciclo. */
    while (!salir)
    {
        XNextEvent(display, &event);

        if (event.type == KeyPress)
        {
            char texto[16];
            KeySym keysym;
            int longitud;

            /* Se pide sizeof(texto)-1, no sizeof(texto): asi siempre
               queda un byte libre para el caracter nulo que se
               agrega despues, y XLookupString nunca puede devolver
               mas caracteres de los que el buffer soporta. */
            longitud = XLookupString(&event.xkey,
                                      texto,
                                      sizeof(texto) - 1,
                                      &keysym,
                                      NULL);

            if (keysym == XK_Return)
            {
                if (send(socketServidor, "\n", 1, 0) < 0)
                {
                    perror("send");
                    salir = 1;
                }
            }
            else if (longitud > 0)
            {
                texto[longitud] = '\0';

                printf("%s", texto);
                fflush(stdout);

                if (send(socketServidor, texto, longitud, 0) < 0)
                {
                    perror("send");
                    salir = 1;
                }
            }

            if (keysym == XK_Escape)
            {
                salir = 1;
            }
        }
        else if (event.type == ClientMessage)
        {
            if ((Atom)event.xclient.data.l[0] == wmDelete)
            {
                salir = 1;
            }
        }
    }
}

void cerrarVentana(Display *display, Window window)
{
    XDestroyWindow(display, window);
    XCloseDisplay(display);
}

int main(int argc, char *argv[])
{
    const char *host = SERVER_IP_DEFECTO;
    const char *puerto = SERVER_PORT_DEFECTO;
    int idVentana = 0;   /* 0 = "sin id asignado" (por ejemplo, si se corre a mano sin el launcher) */
    int idLauncher = 0;  /* 0 = "sin launcher identificado" */
    int socketServidor;
    Display *display;
    Window window;
    Atom wmDelete;

    /* El launcher pasa host, puerto, idVentana e idLauncher como
       argumentos. Si se ejecuta "window" a mano sin argumentos, se
       usan los valores por defecto (solo para pruebas locales). */
    if (argc >= 2)
    {
        host = argv[1];
    }
    if (argc >= 3)
    {
        puerto = argv[2];
    }
    if (argc >= 4)
    {
        idVentana = atoi(argv[3]);
    }
    if (argc >= 5)
    {
        idLauncher = atoi(argv[4]);
    }

    socketServidor = conectarServidor(host, puerto);

    if (socketServidor < 0)
    {
        fprintf(stderr, "No se pudo conectar a IALearner en %s:%s\n", host, puerto);
        return EXIT_FAILURE;
    }

    /* Linea de identificacion ("verificador"): se manda ANTES que
       cualquier tecla, con AMBOS ids -- el de la ventana y el del
       launcher que la creo. Asi ialearner sabe no solo de que
       ventana viene cada oracion, sino tambien a que "computadora"
       (launcher) pertenece, para mantener cada contexto separado. */
    {
        char identificacion[64];
        int longitud = snprintf(identificacion, sizeof(identificacion),
                                 "VENTANA %d LAUNCHER %d\n", idVentana, idLauncher);

        if (longitud > 0 && send(socketServidor, identificacion, (size_t)longitud, 0) < 0)
        {
            perror("send (identificacion)");
        }
    }

    display = iniciarDisplay();
    window = crearVentana(display, &wmDelete);

    procesarEventos(display, wmDelete, socketServidor);

    close(socketServidor);
    cerrarVentana(display, window);

    return 0;
}
