# Agentic-OS-V2 — Segundo Parcial

## Requisitos, instalación

- Linux con sesion grafica (X11, o Wayland con XWayland -- en WSL se
  necesita WSLg o un servidor X externo como VcXsrv).
- gcc y make.
- Cabeceras de desarrollo de X11:

```bash
  sudo apt-get update
  sudo apt-get install build-essential libx11-dev
```

## Compilación

Desde la carpeta raíz del proyecto:

```bash
make
```

Para eliminar los ejecutables:

```bash
make clean
```

## Ejecución (entrar en la carpeta bin y ejecutar ./ejecutable)

### 1. Arrancar el "data center" (`ialearner`)

```bash
./ialearner [-p numeroHilos] [puerto]
```

- `-p numeroHilos` es la cantidad de hilos de detección (P). Si no se
  indica, se usa por defecto la cantidad de CPUs detectados en la compu o laptop.
- `puerto` — puerto TCP donde escucha. Por defecto `5000`.


Al arrancar muestra algo como:


### 2. Ejecutar uno o más launcher para simular dispositivos

En otra terminal:

```bash
./launcher [host] [puerto]
```

`host` y `puerto` son los valores **por defecto** que se sugieren al
crear ventanas (se pueden dejar en blanco con Enter en el menú para
usarlos, o escribir otros distintos para simular otra "computadora",
si hay otro `ialearner` corriendo en otro puerto).

```bash
./launcher                      # se conecta por defecto a 127.0.0.1:5000
./launcher 127.0.0.1 6000       # se conecta por otro puerto
```

Desde el menú del launcher:
1. Crear ventanas
2. Ver estado de procesos
3. Terminar todas las ventanas activas
4. Salir

Al elegir "1", se pide la cantidad de ventanas y, para cada lote, el
host/puerto de la "computadora remota" (Enter para usar el de por
defecto)

Cada **launcher** representa una computadora/usuario independiente.
Se pueden correr varios launchers (en varias terminales) apuntando al
mismo `ialearner`

```bash
# Terminal 1
./ialearner -p numeroHilos puerto

# Terminal 2 (computadora 1)
./launcher

# Terminal 3 (computadora 2)
./launcher
```

### 4. Usar las ventanas

En cada ventana: escribe texto y presiona **Enter** para cerrar una
oración
